// Advancements screen (L): AdvancementsScreen / AdvancementTab / AdvancementWidget over the tree the server sends
// (update_advancements: advancements with display info and positions, progress per criterion), tabs per root,
// tiled backgrounds, connections, obtained / unobtained frames, the hover box with title, progress and description,
// dragging / wheel scrolling, select_advancements_tab and seen_advancements.
#include "audio/audio.h"
#include "ui/frontend.h"
#include "ui/toasts.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ui {

namespace {
constexpr int kInW = 234, kInH = 113;  // WINDOW_INSIDE_WIDTH / HEIGHT
constexpr u32 kLabel = 0xFF404040;

struct Adv {
    std::string id, parent;  // parent "" for roots
    bool hasDisplay = false;
    Text title, description;
    ItemStack icon;
    int type = 0;  // AdvancementType: task, challenge, goal
    bool hidden = false, showToast = true;
    std::string background;  // texture id path ("gui/advancements/backgrounds/stone"), "" none
    float x = 0, y = 0;
    std::vector<std::vector<std::string>> requirements;
};
struct Progress {
    std::unordered_map<std::string, bool> criteria;  // name -> obtained
};

struct Widget {
    const Adv* adv = nullptr;
    int x = 0, y = 0, width = 0;
    std::vector<Text> titleLines, description;
    int parent = -1;
    std::vector<int> children;
};
struct Tab {
    std::string root;
    int index = 0;
    std::vector<Widget> widgets;  // [0] the root
    std::unordered_map<std::string, int> byId;
    double scrollX = 0, scrollY = 0;
    int minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
    bool centered = false;
};

struct Model {
    std::vector<std::string> order;  // insertion order (AdvancementTree)
    std::unordered_map<std::string, Adv> advs;
    std::unordered_map<std::string, Progress> progress;
    std::string selected;  // the selected tab's root
    bool dirty = true;
    std::vector<Tab> tabs;
    float fade = 0;
    int hovered = -1;  // widget of the selected tab
    bool scrolling = false, dragging = false;
    float lastMouse[2] = {};
    bool returnToPause = false;
};
Model g;

// AdvancementProgress over the advancement's requirements.
const Progress* progressOf(const std::string& id) {
    auto it = g.progress.find(id);
    return it == g.progress.end() ? nullptr : &it->second;
}
int completed(const Adv& a, const Progress& p) {
    int n = 0;
    for (const auto& group : a.requirements) {
        bool any = false;
        for (const std::string& c : group)
            if (auto it = p.criteria.find(c); it != p.criteria.end() && it->second) any = true;
        n += any;
    }
    return n;
}
float percent(const Adv& a) {
    const Progress* p = progressOf(a.id);
    if (!p || p->criteria.empty() || a.requirements.empty()) return 0;
    return float(completed(a, *p)) / float(a.requirements.size());
}
bool isDone(const Adv& a) {
    const Progress* p = progressOf(a.id);
    return p && !a.requirements.empty() && completed(a, *p) == int(a.requirements.size());
}
bool shown(const Adv& a) { return !a.hidden || isDone(a); }

std::string rootOf(const std::string& id) {
    std::string cur = id;
    for (int guard = 0; guard < 256; ++guard) {
        auto it = g.advs.find(cur);
        if (it == g.advs.end() || it->second.parent.empty() || !g.advs.count(it->second.parent)) return cur;
        cur = it->second.parent;
    }
    return cur;
}

// StringSplitter-based findOptimalLines: the split whose widest line is closest to the preferred width.
std::vector<Text> optimalLines(UiState& s, const Text& t, int preferred) {
    static const int kOffsets[5] = {0, 10, -10, 25, -25};
    std::vector<Text> best;
    float bestDist = 1e30f;
    for (int m : kOffsets) {
        std::vector<Text> split = s.font.split(t, preferred - m);
        float w = 0;
        for (const Text& l : split) w = std::max(w, s.font.widthF(l));
        const float d = std::abs(w - float(preferred));
        if (d <= 10.0f) return split;
        if (d < bestDist) bestDist = d, best = split;
    }
    return best;
}

Widget makeWidget(UiState& s, const Adv& a) {
    Widget w;
    w.adv = &a;
    w.x = int(std::floor(a.x * 28.0f)), w.y = int(std::floor(a.y * 27.0f));
    w.titleLines = s.font.split(a.title, 163);
    int titleWidth = 80;
    for (const Text& l : w.titleLines) titleWidth = std::max(titleWidth, s.font.width(l));
    int progressWidth = 0;
    if (a.requirements.size() > 1)
        progressWidth = s.font.width(translatable("advancements.progress",
                                                  {Text(std::to_string(a.requirements.size())), Text(std::to_string(a.requirements.size()))},
                                                  s.lang())) + 8;
    int longest = 29 + titleWidth + progressWidth;
    w.description = optimalLines(s, a.description.withStyle(colorStyle(a.type == 1 ? 0xAA00AA : 0x55FF55)), longest);
    for (const Text& l : w.description) longest = std::max(longest, s.font.width(l));
    w.width = longest + 3 + 5;
    return w;
}

// AdvancementsScreen.onAdvancementsUpdated: tabs for roots with a background, widgets for displayed tasks.
void rebuild(UiState& s) {
    std::unordered_map<std::string, Tab> old;
    for (Tab& t : g.tabs) old[t.root] = std::move(t);
    g.tabs.clear();
    for (const std::string& id : g.order) {
        const Adv& a = g.advs[id];
        if (!a.parent.empty() && g.advs.count(a.parent)) continue;
        if (!a.hasDisplay || a.background.empty() || g.tabs.size() >= 8 + 8 + 5 + 5) continue;
        Tab t;
        t.root = id;
        t.index = int(g.tabs.size());
        if (auto it = old.find(id); it != old.end()) {
            t.scrollX = it->second.scrollX, t.scrollY = it->second.scrollY, t.centered = it->second.centered;
        }
        g.tabs.push_back(std::move(t));
    }
    auto tabFor = [&](const std::string& root) -> Tab* {
        for (Tab& t : g.tabs)
            if (t.root == root) return &t;
        return nullptr;
    };
    auto add = [&](Tab& t, const Adv& a) {
        t.byId[a.id] = int(t.widgets.size());
        t.widgets.push_back(makeWidget(s, a));
        const Widget& w = t.widgets.back();
        t.minX = std::min(t.minX, w.x), t.maxX = std::max(t.maxX, w.x + 28);
        t.minY = std::min(t.minY, w.y), t.maxY = std::max(t.maxY, w.y + 27);
    };
    for (Tab& t : g.tabs) add(t, g.advs[t.root]);
    for (const std::string& id : g.order) {
        const Adv& a = g.advs[id];
        if (a.parent.empty() || !g.advs.count(a.parent) || !a.hasDisplay) continue;
        if (Tab* t = tabFor(rootOf(id))) add(*t, a);
    }
    for (Tab& t : g.tabs)  // attachToParent: the first ancestor with a display
        for (size_t i = 0; i < t.widgets.size(); ++i) {
            std::string p = t.widgets[i].adv->parent;
            for (int guard = 0; guard < 256 && !p.empty(); ++guard) {
                auto it = g.advs.find(p);
                if (it == g.advs.end()) break;
                if (it->second.hasDisplay) break;
                p = it->second.parent;
            }
            auto w = t.byId.find(p);
            if (!p.empty() && w != t.byId.end() && w->second != int(i)) {
                t.widgets[i].parent = w->second;
                t.widgets[size_t(w->second)].children.push_back(int(i));
            }
        }
    bool found = false;
    for (const Tab& t : g.tabs) found |= t.root == g.selected;
    if (!found) g.selected = g.tabs.empty() ? "" : g.tabs[0].root;
    g.hovered = -1;
    g.dirty = false;
}

Tab* selectedTab() {
    for (Tab& t : g.tabs)
        if (t.root == g.selected) return &t;
    return nullptr;
}

void sendSeen(UiState& s, const std::string* tab) {  // ServerboundSeenAdvancementsPacket
    if (!s.online()) return;
    net::Writer w;
    w.varInt(tab ? 0 : 1);  // OPENED_TAB / CLOSED_SCREEN
    if (tab) w.str(*tab);
    s.client->sendPlay("seen_advancements", w);
}

void selectTab(UiState& s, const std::string& root, bool tellServer) {  // ClientAdvancements.setSelectedTab
    if (tellServer && !root.empty()) sendSeen(s, &root);  // every time, changed or not
    g.selected = root;
}

// AdvancementTabType: ABOVE (8), BELOW (8), LEFT (5), RIGHT (5).
struct TabPlace {
    int type, i;  // type 0..3, index within it
};
TabPlace place(int index) {
    static const int kMax[4] = {8, 8, 5, 5};
    for (int t = 0; t < 4; ++t) {
        if (index < kMax[t]) return {t, index};
        index -= kMax[t];
    }
    return {3, 4};
}
void tabRect(int index, int& x, int& y, int& w, int& h) {
    const TabPlace p = place(index);
    if (p.type < 2) w = 28, h = 32;
    else w = 32, h = 28;
    switch (p.type) {
        case 0: x = (w + 4) * p.i, y = -h + 4; break;
        case 1: x = (w + 4) * p.i, y = 136; break;
        case 2: x = -w + 4, y = h * p.i; break;
        default: x = 248, y = h * p.i; break;
    }
}

// --- packets ----------------------------------------------------------------------------------------------------

struct Update {
    bool reset = false, showToasts = false;
    std::vector<Adv> added;
    std::vector<std::string> removed;
    std::vector<std::pair<std::string, Progress>> progress;
};

void handlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    const Items* items = &s.items;
    const Lang* lang = &s.res.lang();
    client.onPlay("update_advancements", [sp, items, lang](net::Reader& r) {
        auto u = std::make_shared<Update>();
        u->reset = r.boolean();
        const i32 n = r.varInt();
        for (i32 i = 0; i < n && r.ok; ++i) {
            Adv a;
            a.id = std::string(r.str());
            if (r.boolean()) a.parent = std::string(r.str());
            if (r.boolean()) {  // DisplayInfo
                a.hasDisplay = true;
                a.title = parseComponent(r.nbt(), lang);
                a.description = parseComponent(r.nbt(), lang);
                // ItemStackTemplate: item, count, component patch (decoded through Items::read: count first).
                const i32 item = r.varInt(), count = r.varInt();
                net::Writer head;
                head.varInt(count), head.varInt(item);
                std::string buf(head.buf.begin(), head.buf.end());
                buf.append(reinterpret_cast<const char*>(r.p), r.left());
                net::Reader sub(reinterpret_cast<const u8*>(buf.data()), buf.size());
                if (!items->read(sub, a.icon)) {
                    LOG("ui: advancement %s: icon components not decodable", a.id.c_str());
                    return;
                }
                r.skip(size_t(sub.p - reinterpret_cast<const u8*>(buf.data())) - head.buf.size());
                a.type = r.varInt();
                const i32 flags = r.i32v();
                if (flags & 1) {
                    std::string bg(r.str());
                    a.background = bg.substr(bg.find(':') + 1);
                }
                a.showToast = (flags & 2) != 0;
                a.hidden = (flags & 4) != 0;
            }
            const i32 groups = r.varInt();
            for (i32 gi = 0; gi < groups && r.ok; ++gi) {
                std::vector<std::string> group;
                const i32 m = r.varInt();
                for (i32 k = 0; k < m && r.ok; ++k) group.emplace_back(r.str());
                a.requirements.push_back(std::move(group));
            }
            r.boolean();  // sendsTelemetryEvent
            a.x = r.f32(), a.y = r.f32();  // PositionedAdvancement: the server lays the tree out
            u->added.push_back(std::move(a));
        }
        const i32 removed = r.varInt();
        for (i32 i = 0; i < removed && r.ok; ++i) u->removed.emplace_back(r.str());
        const i32 progress = r.varInt();
        for (i32 i = 0; i < progress && r.ok; ++i) {
            std::pair<std::string, Progress> p;
            p.first = std::string(r.str());
            const i32 c = r.varInt();
            for (i32 k = 0; k < c && r.ok; ++k) {
                std::string name(r.str());
                const bool obtained = r.boolean();
                if (obtained) r.i64v();  // Instant (epoch milliseconds)
                p.second.criteria[name] = obtained;
            }
            u->progress.push_back(std::move(p));
        }
        u->showToasts = r.boolean();  // showAdvancements (toasts)
        if (!r.ok) {
            LOG("ui: update_advancements: malformed packet");
            return;
        }
        sp->post([u] {
            if (u->reset) g.advs.clear(), g.order.clear(), g.progress.clear();
            for (const std::string& id : u->removed) {
                g.advs.erase(id);
                g.order.erase(std::remove(g.order.begin(), g.order.end(), id), g.order.end());
            }
            for (Adv& a : u->added) {
                if (!g.advs.count(a.id)) g.order.push_back(a.id);
                g.advs[a.id] = std::move(a);
            }
            for (auto& [id, p] : u->progress) {
                // ClientAdvancements.update: an advancement just done shows its toast (when the packet allows it).
                const auto it = g.advs.find(id);
                const bool wasDone = it != g.advs.end() && isDone(it->second);
                g.progress[id] = std::move(p);
                if (it != g.advs.end() && !u->reset && u->showToasts && !wasDone && isDone(it->second) &&
                    it->second.hasDisplay && it->second.showToast)
                    showAdvancementToast(it->second.title, it->second.type, it->second.icon);
            }
            g.dirty = true;
        });
    });
    client.onPlay("select_advancements_tab", [sp](net::Reader& r) {
        std::string tab;
        if (r.boolean()) tab = std::string(r.str());
        sp->post([tab] { g.selected = tab; });
    });
}

void attached(UiState&) {
    g.advs.clear(), g.order.clear(), g.progress.clear(), g.tabs.clear();
    g.selected.clear();
    g.dirty = true;
}

void tick(UiState& s) {  // AdvancementTab.tick: hover and the fade
    if (s.screen != Screen::Advancements) return;
    Tab* t = selectedTab();
    if (!t) return;
    const int left = (s.guiW() - 252) / 2, top = (s.guiH() - 140) / 2;
    const int mx = int(s.mouseX) - left - 9, my = int(s.mouseY) - top - 18;
    bool hovering = false;
    if (mx > 0 && mx < kInW && my > 0 && my < kInH) {
        const int sx = int(std::floor(t->scrollX)), sy = int(std::floor(t->scrollY));
        for (size_t i = 0; i < t->widgets.size(); ++i) {
            const Widget& w = t->widgets[i];
            if (!shown(*w.adv)) continue;
            const int x0 = sx + w.x, y0 = sy + w.y;
            if (mx >= x0 && mx <= x0 + 26 && my >= y0 && my <= y0 + 26) {
                hovering = true;
                g.hovered = int(i);
                break;
            }
        }
    }
    if (hovering) g.fade = std::clamp(g.fade + 0.06f, 0.0f, 0.3f);
    else g.fade = std::clamp(g.fade - 0.12f, 0.0f, 1.0f), g.hovered = -1;
}

bool worldKey(UiState& s, const InputEvent& e) {
    if (e.repeat || !s.isKey(Key::Advancements, e) || !s.online()) return false;
    g.returnToPause = false;
    s.openScreen(Screen::Advancements);
    return true;
}

const UiModuleRegistrar kModule({"advancements", handlers, attached, tick, nullptr, nullptr, worldKey});

// --- screen -------------------------------------------------------------------------------------------------------

void close(UiState& s) {
    if (g.returnToPause) s.openScreen(Screen::Pause);
    else s.closeScreen(false);
}

void build(UiState& s) {
    s.buttons.clear();
    Button done;  // HeaderAndFooterLayout footer: Done (200 wide)
    done.w = 200, done.x = float(s.guiW() / 2 - 100), done.y = float(s.guiH() - 33 + 6);
    done.label = Text(s.res.translate("gui.done"));
    UiState* sp = &s;
    done.onPress = [sp] { close(*sp); };
    s.buttons.push_back(std::move(done));
    if (g.dirty) rebuild(s);
    // AdvancementsScreen.init: new tabs (centred again), the first one selected (onAdvancementsUpdated tells the
    // server, then init once more)
    for (Tab& t : g.tabs) t.centered = false, t.scrollX = t.scrollY = 0;
    g.hovered = -1, g.fade = 0;
    if (!g.tabs.empty()) {
        g.selected = g.tabs[0].root;
        sendSeen(s, &g.selected);
        sendSeen(s, &g.selected);
    }
}

void closed(UiState& s) {
    sendSeen(s, nullptr);  // removed(): CLOSED_SCREEN
    g.scrolling = g.dragging = false;
}

void drawWidgetLinks(Canvas& c, const Tab& t, int i, int ox, int oy, bool background) {  // extractConnectivity
    const Widget& w = t.widgets[size_t(i)];
    if (w.parent >= 0) {
        const Widget& p = t.widgets[size_t(w.parent)];
        const int depX = ox + p.x + 13, splitX = ox + p.x + 26 + 4, depY = oy + p.y + 13;
        const int myX = ox + w.x + 13, myY = oy + w.y + 13;
        const u32 col = background ? 0xFF000000 : 0xFFFFFFFF;
        auto hline = [&](int x0, int x1, int y) {
            if (x1 < x0) std::swap(x0, x1);
            c.fill(float(x0), float(y), float(x1 + 1), float(y + 1), col);
        };
        auto vline = [&](int x, int y0, int y1) {  // GuiGraphics.verticalLine: exclusive ends
            if (y1 < y0) std::swap(y0, y1);
            c.fill(float(x), float(y0 + 1), float(x + 1), float(y1), col);
        };
        if (background) {
            hline(splitX, depX, depY - 1), hline(splitX + 1, depX, depY), hline(splitX, depX, depY + 1);
            hline(myX, splitX - 1, myY - 1), hline(myX, splitX - 1, myY), hline(myX, splitX - 1, myY + 1);
            vline(splitX - 1, myY, depY), vline(splitX + 1, myY, depY);
        } else {
            hline(splitX, depX, depY), hline(myX, splitX, myY), vline(splitX, myY, depY);
        }
    }
    for (int ch : w.children) drawWidgetLinks(c, t, ch, ox, oy, background);
}

const char* frameSprite(bool obtained, int type) {
    static const char* kObt[3] = {"advancements/task_frame_obtained", "advancements/challenge_frame_obtained", "advancements/goal_frame_obtained"};
    static const char* kUn[3] = {"advancements/task_frame_unobtained", "advancements/challenge_frame_unobtained", "advancements/goal_frame_unobtained"};
    return (obtained ? kObt : kUn)[std::clamp(type, 0, 2)];
}

void drawWidgets(UiState& s, Canvas& c, const Tab& t, int i, int ox, int oy) {
    const Widget& w = t.widgets[size_t(i)];
    if (shown(*w.adv)) {
        blitSprite(c, s.sprites.sprite(frameSprite(percent(*w.adv) >= 1.0f, w.adv->type)), float(ox + w.x + 3), float(oy + w.y), 26, 26);
        s.drawItem(c, w.adv->icon, float(ox + w.x + 8), float(oy + w.y + 5), false);
    }
    for (int ch : w.children) drawWidgets(s, c, t, ch, ox, oy);
}

// AdvancementWidget.extractHover (relative to the window inside at ox, oy).
void drawHover(UiState& s, Canvas& c, const Widget& w, int xo, int yo, int screenX, int insideY) {
    const Adv& a = *w.adv;
    const int titleBarHeight = 9 * int(w.titleLines.size()) + 9 + 8;
    const int titleTop = yo + w.y + (26 - titleBarHeight) / 2, titleBarBottom = titleTop + titleBarHeight;
    const int descTextHeight = int(w.description.size()) * 9, descHeight = 6 + descTextHeight;
    const bool leftSide = screenX + xo + w.x + w.width + 26 >= s.guiW();
    Text progressText;
    bool hasProgress = false;
    if (const Progress* p = progressOf(a.id); p && !p->criteria.empty() && a.requirements.size() > 1) {
        progressText = translatable("advancements.progress",
                                    {Text(std::to_string(completed(a, *p))), Text(std::to_string(a.requirements.size()))}, s.lang());
        hasProgress = true;
    }
    const int progressWidth = hasProgress ? s.font.width(progressText) : 0;
    const bool topSide = titleBarBottom - insideY + descHeight >= kInH;  // relative to the window's inside
    const float amount = percent(a);
    int firstHalfWidth = int(std::floor(amount * float(w.width)));
    bool firstObt, secondObt, iconObt;
    if (amount >= 1.0f) firstHalfWidth = w.width / 2, firstObt = secondObt = iconObt = true;
    else if (firstHalfWidth < 2) firstHalfWidth = w.width / 2, firstObt = secondObt = iconObt = false;
    else if (firstHalfWidth > w.width - 2) firstHalfWidth = w.width / 2, firstObt = secondObt = true, iconObt = false;
    else firstObt = true, secondObt = false, iconObt = false;
    const int secondBarWidth = w.width - firstHalfWidth;
    const int titleLeft = leftSide ? xo + w.x - w.width + 26 + 6 : xo + w.x;
    const int bgHeight = titleBarHeight + descHeight;
    if (!w.description.empty())
        blitSprite(c, s.sprites.sprite("advancements/title_box"), float(titleLeft), float(topSide ? titleBarBottom - bgHeight : titleTop),
                   float(w.width), float(bgHeight));
    auto box = [&](bool obtained) { return s.sprites.sprite(obtained ? "advancements/box_obtained" : "advancements/box_unobtained"); };
    if (firstObt != secondObt) {  // blitSprite with a texture region: the 200-wide box clipped to each half
        c.pushScissor(float(titleLeft), float(titleTop), float(titleLeft + firstHalfWidth), float(titleTop + titleBarHeight));
        blitSprite(c, box(firstObt), float(titleLeft), float(titleTop), 200, float(titleBarHeight));
        c.popScissor();
        c.pushScissor(float(titleLeft + firstHalfWidth), float(titleTop), float(titleLeft + w.width), float(titleTop + titleBarHeight));
        blitSprite(c, box(secondObt), float(titleLeft + firstHalfWidth - (200 - secondBarWidth)), float(titleTop), 200, float(titleBarHeight));
        c.popScissor();
    } else {
        blitSprite(c, box(firstObt), float(titleLeft), float(titleTop), float(w.width), float(titleBarHeight));
    }
    blitSprite(c, s.sprites.sprite(frameSprite(iconObt, a.type)), float(xo + w.x + 3), float(yo + w.y), 26, 26);
    const int descLeft = titleLeft + 5;
    auto lines = [&](const std::vector<Text>& v, int x, int y, u32 color) {
        for (size_t i = 0; i < v.size(); ++i) s.drawText(c, v[i], float(x), float(y + int(i) * 9), color, true);
    };
    if (leftSide) {
        lines(w.titleLines, descLeft, titleTop + 9, 0xFFFFFFFF);
        if (hasProgress) s.drawText(c, progressText, float(xo + w.x - progressWidth), float(titleTop + 9), 0xFFFFFFFF, true);
    } else {
        lines(w.titleLines, xo + w.x + 32, titleTop + 9, 0xFFFFFFFF);
        if (hasProgress) s.drawText(c, progressText, float(xo + w.x + w.width - progressWidth - 5), float(titleTop + 9), 0xFFFFFFFF, true);
    }
    lines(w.description, descLeft, topSide ? titleTop - descTextHeight + 1 : titleBarBottom, 0xFF00FF00);
    s.drawItem(c, a.icon, float(xo + w.x + 8), float(yo + w.y + 5), false);
}

void draw(UiState& s, Canvas& c) {
    if (g.dirty) rebuild(s);
    const int W = c.guiWidth(), H = c.guiHeight();
    const int left = (W - 252) / 2, top = (H - 140) / 2;
    const float mx = s.mouseX, my = s.mouseY;
    menuBackground(s, c);  // Screen.extractBackground
    s.drawCentered(c, Text(s.res.translate("gui.advancements")), float(W / 2), float((33 - 9) / 2), 0xFFFFFFFF);
    Tab* t = selectedTab();
    // Dragging (mouseDragged with the left button).
    if (g.dragging && s.leftDown && t) {
        const double dx = mx - g.lastMouse[0], dy = my - g.lastMouse[1];
        if (g.scrolling) {
            if (t->maxX - t->minX > kInW) t->scrollX = std::clamp(t->scrollX + dx, double(-(t->maxX - kInW)), 0.0);
            if (t->maxY - t->minY > kInH) t->scrollY = std::clamp(t->scrollY + dy, double(-(t->maxY - kInH)), 0.0);
        }
        if (dx != 0 || dy != 0) g.scrolling = true;
    }
    if (!s.leftDown) g.dragging = g.scrolling = false;
    g.lastMouse[0] = mx, g.lastMouse[1] = my;
    if (g.scrolling && t) {  // AdvancementsScreen: the axes the tab scrolls along
        const bool h = t->maxX - t->minX > kInW, v = t->maxY - t->minY > kInH;
        if (h || v) s.requestCursor(h && v ? UiState::CursorResizeAll : h ? UiState::CursorResizeEW : UiState::CursorResizeNS);
    }
    // Inside.
    const int inX = left + 9, inY = top + 18;
    if (!t) {
        c.fill(float(inX), float(inY), float(inX + kInW), float(inY + kInH), 0xFF000000);
        s.drawCentered(c, Text(s.res.translate("advancements.empty")), float(inX + 117), float(inY + 56 - 9 / 2), 0xFFFFFFFF);
        s.drawCentered(c, Text(s.res.translate("advancements.sad_label")), float(inX + 117), float(inY + kInH - 9), 0xFFFFFFFF);
    } else {
        if (!t->centered) {
            t->scrollX = 117 - (t->maxX + t->minX) / 2;
            t->scrollY = 56 - (t->maxY + t->minY) / 2;
            t->centered = true;
        }
        c.pushScissor(float(inX), float(inY), float(inX + kInW), float(inY + kInH));
        const int sx = int(std::floor(t->scrollX)), sy = int(std::floor(t->scrollY));
        const Sprite* bg = s.sprites.texture(g.advs[t->root].background);
        const int l0 = sx % 16, t0 = sy % 16;
        for (int x = -1; x <= 15; ++x)
            for (int y = -1; y <= 8; ++y)
                blitTexture(c, bg, float(inX + l0 + 16 * x), float(inY + t0 + 16 * y), 0, 0, 16, 16, 16, 16);
        drawWidgetLinks(c, *t, 0, inX + sx, inY + sy, true);
        drawWidgetLinks(c, *t, 0, inX + sx, inY + sy, false);
        drawWidgets(s, c, *t, 0, inX + sx, inY + sy);
        c.popScissor();
    }
    // Window, tabs, title.
    blitTexture(c, s.sprites.texture("gui/advancements/window"), float(left), float(top), 0, 0, 252, 140, 256, 256);
    if (g.tabs.size() > 1) {
        static const char* kKind[4] = {"above", "below", "left", "right"};
        static const char* kFirst[4] = {"left", "left", "top", "top"};
        static const char* kLast[4] = {"right", "right", "bottom", "bottom"};
        static const int kMax[4] = {8, 8, 5, 5};
        for (const Tab& tb : g.tabs) {
            const TabPlace p = place(tb.index);
            int x, y, w, h;
            tabRect(tb.index, x, y, w, h);
            const char* pos = p.i == 0 ? kFirst[p.type] : p.i == kMax[p.type] - 1 ? kLast[p.type] : "middle";
            const std::string sp = std::string("advancements/tab_") + kKind[p.type] + "_" + pos + (tb.root == g.selected ? "_selected" : "");
            blitSprite(c, s.sprites.sprite(sp), float(left + x), float(top + y), float(w), float(h));
            if (tb.root != g.selected && mx > left + x && my > top + y && mx < left + x + w && my < top + y + h)
                s.requestCursor(UiState::CursorPointingHand);  // AdvancementTab.extractTab
        }
        for (const Tab& tb : g.tabs) {
            const TabPlace p = place(tb.index);
            int x, y, w, h;
            tabRect(tb.index, x, y, w, h);
            static const int kIconOff[4][2] = {{6, 9}, {6, 6}, {10, 5}, {6, 5}};
            s.drawItem(c, g.advs[tb.root].icon, float(left + x + kIconOff[p.type][0]), float(top + y + kIconOff[p.type][1]), false);
        }
    }
    s.drawText(c, t ? g.advs[t->root].title : Text(s.res.translate("gui.advancements")), float(left + 8), float(top + 6), kLabel, false);
    s.drawButtons(c);
    // Tooltips: the fade over the inside, the hovered widget's box, tab titles.
    if (t) {
        c.fill(float(inX), float(inY), float(inX + kInW), float(inY + kInH), u32(std::floor(g.fade * 255.0f)) << 24);
        if (g.hovered >= 0 && size_t(g.hovered) < t->widgets.size())
            drawHover(s, c, t->widgets[size_t(g.hovered)], inX + int(std::floor(t->scrollX)), inY + int(std::floor(t->scrollY)), -9, inY);
    }
    if (g.tabs.size() > 1)
        for (const Tab& tb : g.tabs) {
            int x, y, w, h;
            tabRect(tb.index, x, y, w, h);
            if (mx > float(left + x) && mx < float(left + x + w) && my > float(top + y) && my < float(top + y + h))
                s.drawTooltip(c, {g.advs[tb.root].title}, mx, my);
        }
}

void input(UiState& s, const InputEvent& e) {
    if ((e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) || s.isKey(Key::Advancements, e)) {
        if (s.isKey(Key::Advancements, e) && !g.returnToPause) return s.closeScreen(false);
        return close(s);
    }
    if (s.buttonsInput(e)) return;
    Tab* t = selectedTab();
    if (e.type == InputEvent::Wheel && t) {  // mouseScrolled: vertical wheel moves the view 16 per notch
        if (t->maxY - t->minY > kInH) t->scrollY = std::clamp(t->scrollY + e.code * 16.0, double(-(t->maxY - kInH)), 0.0);
        return;
    }
    if (e.type != InputEvent::Button || e.code != 0) return;
    if (!e.down) {
        g.dragging = g.scrolling = false;
        return;
    }
    g.dragging = true;
    g.lastMouse[0] = s.mouseX, g.lastMouse[1] = s.mouseY;
    const int left = (s.guiW() - 252) / 2, top = (s.guiH() - 140) / 2;
    if (g.tabs.size() > 1)
        for (const Tab& tb : g.tabs) {
            int x, y, w, h;
            tabRect(tb.index, x, y, w, h);
            if (s.mouseX > float(left + x) && s.mouseX < float(left + x + w) && s.mouseY > float(top + y) && s.mouseY < float(top + y + h)) {
                selectTab(s, tb.root, true);
                return;
            }
        }
}

const ScreenRegistrar kScreen({Screen::Advancements, build, draw, input, closed});
}  // namespace

// The pause menu's Advancements button (screens.cpp).
void openAdvancementsFromPause(UiState& s) {
    s.openScreen(Screen::Advancements);
    g.returnToPause = true;
}

}  // namespace ui
