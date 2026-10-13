// Social Interactions (SocialInteractionsScreen, SocialInteractionsPlayerList, PlayerEntry): the players online and
// seen this session, hiding their chat messages (PlayerSocialManager: this session only). Blocking and reporting
// need a Microsoft account: the Blocked tab stays empty and the report buttons disabled.
#include "audio/audio.h"
#include "net/client.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/mp_hud.h"
#include "ui/narrator.h"
#include "ui/options_list.h"
#include "ui/sprites.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace ui {
namespace {

struct Entry {
    PlayerInfoView p;
    bool removed = false;  // offline
};
struct State {
    int page = 0;  // 0 all, 1 hidden, 2 blocked
    Field search;
    std::string filter;
    float scroll = 0;
    std::vector<Entry> entries;
    std::unordered_set<std::string> hidden;  // PlayerSocialManager.hiddenPlayers
    Screen back = Screen::None;
    size_t onlineCount = 0;
} g;

constexpr int kItem = 36, kListTop = 88, kHeader = 33, kFooter = 33;
int windowHeight(UiState& s) { return std::max(52, s.guiH() - 128 - 16); }
int listEnd(UiState& s) { return 80 + windowHeight(s) - 8; }
int marginX(UiState& s) { return (s.guiW() - 238) / 2; }
float rowLeft(UiState& s) { return float(s.guiW() / 2 - 110); }
bool isLocal(UiState& s, const std::string& uuid) { return s.client && uuid == s.client->playerUuid(); }

std::string lower(std::string v) {
    for (char& ch : v) ch = char(std::tolower(u8(ch)));
    return v;
}

void collect(UiState& s) {  // SocialInteractionsPlayerList.updatePlayerList, sortPlayerEntries, the filter
    g.entries.clear();
    const std::vector<PlayerInfoView> online = playerInfos();
    auto add = [&](const PlayerInfoView& p, bool removed) {
        for (const Entry& e : g.entries)
            if (e.p.uuid == p.uuid) return;
        g.entries.push_back({p, removed});
    };
    if (g.page == 0) {
        for (const PlayerInfoView& p : online) add(p, false);
        for (const PlayerInfoView& p : seenPlayers()) add(p, true);  // the ones still online are in already
    } else if (g.page == 1) {
        for (const PlayerInfoView& p : online)
            if (g.hidden.contains(p.uuid)) add(p, false);
    }
    auto rank = [&](const Entry& e) {
        if (isLocal(s, e.p.uuid)) return 0;
        return e.p.uuid.size() == 16 && (u8(e.p.uuid[6]) >> 4) == 2 ? 4 : 3;
    };
    auto nameClass = [](const std::string& n) { return !n.empty() && (std::isalnum(u8(n[0])) || n[0] == '_') ? 0 : 1; };
    std::stable_sort(g.entries.begin(), g.entries.end(), [&](const Entry& a, const Entry& b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        if (nameClass(a.p.name) != nameClass(b.p.name)) return nameClass(a.p.name) < nameClass(b.p.name);
        return lower(a.p.name) < lower(b.p.name);
    });
    if (!g.filter.empty()) std::erase_if(g.entries, [](const Entry& e) { return lower(e.p.name).find(g.filter) == std::string::npos; });
    const float maxScroll = std::max(0.0f, float(int(g.entries.size()) * kItem + 4) - float(listEnd(s) - kListTop));
    g.scroll = std::clamp(g.scroll, 0.0f, maxScroll);
}

float maxScroll(UiState& s) { return std::max(0.0f, float(int(g.entries.size()) * kItem + 4) - float(listEnd(s) - kListTop)); }

void build(UiState& s) {
    collect(s);
    s.buttons.clear();
    UiState* p = &s;
    const float R = rowLeft(s), rowRight = R + 220;
    const int bw = 220 / 3;
    static const char* kTabs[3] = {"gui.socialInteractions.tab_all", "gui.socialInteractions.tab_hidden", "gui.socialInteractions.tab_blocked"};
    const float xs[3] = {R, float(int(R + rowRight - float(bw)) / 2 + 1), rowRight - float(bw) + 1};
    for (int i = 0; i < 3; ++i) {
        Style st;
        if (g.page == i) st.underlined = 1;  // the selected tab is underlined
        addButton(s, xs[i], 45, float(bw), Text(s.res.translate(kTabs[i]), st), [p, i] {
            g.page = i;
            g.scroll = 0;
            collect(*p);
            p->buildButtons();
            if (g.entries.empty()) {  // showPage: the empty text is spoken
                const char* key = !g.filter.empty() ? "gui.socialInteractions.search_empty"
                                  : i == 1          ? "gui.socialInteractions.empty_hidden"
                                  : i == 2          ? "gui.socialInteractions.empty_blocked"
                                                    : nullptr;
                if (key) narrator::say(p->res.translate(key), true);
            }
        });
    }
    if (g.page == 2)  // Blocking is managed with the Microsoft account
        addButton(s, float(s.guiW() / 2 - 100), float(64 + windowHeight(s)), 200, tr(s, "gui.socialInteractions.blocking_hint"),
                  [p] { openConfirmLink(*p, "https://aka.ms/javablocking", Screen::SocialInteractions); });
    addButton(s, float(s.guiW() / 2 - 100), float(s.guiH() - kFooter + 6), 200, tr(s, "gui.done"), [p] { p->openScreen(g.back); });
    g.search.x = float(marginX(s) + 28), g.search.y = 74, g.search.w = 200, g.search.h = 15;
    g.search.hint = s.res.translate("gui.socialInteractions.search_hint");
    setFields(s, {&g.search}, -1);
}

void open(UiState& s, Screen back) {
    g.back = back;
    g.page = 0, g.scroll = 0;
    g.search.text = {}, g.filter.clear();
    collect(s);
    s.openScreen(Screen::SocialInteractions);
}

// The row's buttons: hide / show at the right, report (disabled) at the very right.
struct RowButtons {
    float hideX, reportX, y;
};
RowButtons rowButtons(UiState& s, float rowTop) {
    const float cx = rowLeft(s) + 2, cw = 216, cy = rowTop, ch = 32;
    return {cx + cw - 48, cx + cw - 24, cy + (ch - 20) / 2};
}

void toggleHidden(UiState& s, const Entry& e) {  // PlayerEntry: hidePlayer / showPlayer, then onHiddenOrShown
    const bool hide = !g.hidden.contains(e.p.uuid);
    if (hide) g.hidden.insert(e.p.uuid);
    else g.hidden.erase(e.p.uuid);
    const Text msg = translatable(hide ? "gui.socialInteractions.hidden_in_chat" : "gui.socialInteractions.shown_in_chat", {Text(e.p.name)}, s.lang());
    s.addChatMessage({msg, s.guiTicks, {}, {}, 0});
    narrator::say(msg.plain(), true);
    if (g.page == 1) collect(s);
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    const int mx = marginX(s), wh = windowHeight(s), le = listEnd(s);
    blitSprite(c, s.sprites.sprite("social_interactions/background"), float(mx + 3), 64, 236, float(wh + 16));
    blitSprite(c, s.sprites.sprite("icon/search"), float(mx + 3 + 10), 76, 12, 12);
    s.drawCentered(c, tr(s, "gui.socialInteractions.title"), float(W / 2), float((kHeader - 9) / 2 + 1), 0xFFFFFFFF);
    // The server label: the world's name in singleplayer, else the server's, with the player count.
    const std::vector<PlayerInfoView> online = playerInfos();
    if (online.size() != g.onlineCount) g.onlineCount = online.size(), collect(s);  // players joined / left
    std::string server = s.fe->cfg.singleplayer ? s.fe->cfg.worldName : s.fe->cfg.serverAddress;
    if (server.empty() && s.fe->cfg.singleplayer) server = s.fe->cfg.worldDir.filename().string();
    const Text label = translatable(online.size() == 1 ? "gui.socialInteractions.server_label.single" : "gui.socialInteractions.server_label.multiple",
                                    {Text(server), Text(std::to_string(online.size()))}, s.lang());
    s.drawText(c, label, float(mx + 8), 35, 0xFFFFFFFF, false);
    std::vector<Text> tooltip;
    if (!g.entries.empty()) {
        const float R = rowLeft(s);
        c.pushScissor(0, float(kListTop + 4), float(W), float(le));
        for (size_t i = 0; i < g.entries.size(); ++i) {
            const float top = float(kListTop + 4) + float(i) * kItem - g.scroll;
            if (top + kItem < kListTop || top > le) continue;
            const Entry& e = g.entries[i];
            const bool hidden = g.hidden.contains(e.p.uuid);
            const char* status = hidden && e.removed ? "gui.socialInteractions.status_hidden_offline"
                                 : hidden             ? "gui.socialInteractions.status_hidden"
                                 : e.removed          ? "gui.socialInteractions.status_offline"
                                                      : nullptr;
            const float cx = R + 2, cw = 216, ch = 32;
            c.fill(cx, top, cx + cw, top + ch, status ? 0xFF303030u : 0xFF4A4A4Au);
            const float fx = cx + 4, fy = top + (ch - 24) / 2;  // PlayerFaceExtractor, 24 pixels
            if (const Sprite* skin = s.sprites.texture(e.p.skin)) {
                c.push();
                c.translate(fx, fy);
                c.scaleBy(3.0f);
                blitTexture(c, skin, 0, 0, 8, 8, 8, 8, 64, 64);
                if (e.p.showHat) blitTexture(c, skin, 0, 0, 40, 8, 8, 8, 64, 64);
                c.pop();
            }
            if (e.removed) c.fill(fx, fy, fx + 24, fy + 24, 0xBE000000u);
            const float nx = fx + 24 + 4;
            const float ny = status ? top + (ch - 18) / 2 : top + (ch - 9) / 2;
            s.drawText(c, Text(e.p.name), nx, ny, 0xFFFFFFFF, false);
            if (status) {
                Style it;
                it.italic = 1;
                s.drawText(c, Text(s.res.translate(status), it), nx, ny + 12, 0x8CFFFFFF, false);
            }
            if (isLocal(s, e.p.uuid)) continue;  // no buttons for ourselves
            const RowButtons b = rowButtons(s, top);
            auto over = [&](float x) { return s.mouseX >= x && s.mouseX < x + 20 && s.mouseY >= b.y && s.mouseY < b.y + 20 && s.mouseY >= kListTop + 4 && s.mouseY < le; };
            const bool hov = over(b.hideX);
            blitSprite(c, s.sprites.sprite(std::string(hidden ? "social_interactions/unmute_button" : "social_interactions/mute_button") + (hov ? "_highlighted" : "")),
                       b.hideX, b.y, 20, 20);
            if (hov) tooltip = {tr(s, hidden ? "gui.socialInteractions.tooltip.show" : "gui.socialInteractions.tooltip.hide")};
            blitSprite(c, s.sprites.sprite("social_interactions/report_button_disabled"), b.reportX, b.y, 20, 20);
            if (over(b.reportX)) tooltip = {tr(s, "gui.socialInteractions.tooltip.report.disabled")};
        }
        c.popScissor();
        if (const float ms = maxScroll(s); ms > 0) {
            const float x = rowLeft(s) + 220 + 4, top = float(kListTop + 4), h = float(le) - top;
            const float bar = std::max(32.0f, h * h / (h + ms)), by = top + (h - bar) * g.scroll / ms;
            blitSprite(c, s.sprites.sprite("widget/scroller_background"), x, top, 6, h);
            blitSprite(c, s.sprites.sprite("widget/scroller"), x, by, 6, bar);
        }
    } else {  // the empty text (gray)
        const char* key = !g.filter.empty() ? "gui.socialInteractions.search_empty"
                          : g.page == 1     ? "gui.socialInteractions.empty_hidden"
                          : g.page == 2     ? "gui.socialInteractions.empty_blocked"
                                            : nullptr;
        if (key) s.drawCentered(c, tr(s, key), float(W / 2), float((72 + le) / 2), 0xFFAAAAAA);
    }
    drawFields(s, c);
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
}

void input(UiState& s, const InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.openScreen(g.back);
    // The Social Interactions key closes it again (not while typing in the search box).
    if (e.type == E::Key && e.down && !e.repeat && s.fe->focus < 0 && s.isKey(Key::SocialInteractions, e)) return s.openScreen(g.back);
    if (e.type == E::Wheel) {
        g.scroll = std::clamp(g.scroll - float(e.code) * float(kItem) / 2.0f, 0.0f, maxScroll(s));
        return;
    }
    const std::string before = g.search.text.value;
    if (fieldsInput(s, e)) {
        if (g.search.text.value.size() > 16) g.search.text.value.resize(16);  // setMaxLength(16)
        if (g.search.text.value != before) {
            const std::string f = lower(g.search.text.value);
            if (f != g.filter) g.filter = f, g.scroll = 0, collect(s);
        }
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.mouseY >= kListTop + 4 && s.mouseY < listEnd(s)) {
        for (size_t i = 0; i < g.entries.size(); ++i) {
            const float top = float(kListTop + 4) + float(i) * kItem - g.scroll;
            if (isLocal(s, g.entries[i].p.uuid)) continue;
            const RowButtons b = rowButtons(s, top);
            if (s.mouseX >= b.hideX && s.mouseX < b.hideX + 20 && s.mouseY >= b.y && s.mouseY < b.y + 20) {
                audio::playUiClick();
                const Entry entry = g.entries[i];
                toggleHidden(s, entry);
                return;
            }
        }
    }
    s.buttonsInput(e);
}

void closed(UiState& s) { setFields(s, {}, -1); }

const ScreenRegistrar kSocial({Screen::SocialInteractions, build, draw, input, closed});

// Gui: the Social Interactions key (multiplayer only).
bool worldKey(UiState& s, const InputEvent& e) {
    if (e.type != Window::InputEvent::Key || !e.down || e.repeat || !s.isKey(Key::SocialInteractions, e)) return false;
    if (s.fe->cfg.singleplayer) {  // only in multiplayer worlds
        const Text msg(s.res.translate("multiplayer.socialInteractions.not_available"));
        s.actionbar = msg, s.actionbarTime = 60, s.actionbarAnimated = false;
        narrator::say(msg.plain(), true);
        return true;
    }
    open(s, Screen::None);
    return true;
}

void attached(UiState&) { g.hidden.clear(); }  // per session

const UiModuleRegistrar kModule({.name = "social", .attached = attached, .worldKey = worldKey});

}  // namespace

bool socialHidden(const std::string& uuid) { return g.hidden.contains(uuid); }
void openSocialInteractions(UiState& s, Screen back) { open(s, back); }

}  // namespace ui
