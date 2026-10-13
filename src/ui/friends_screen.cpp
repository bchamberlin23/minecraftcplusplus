// Friends (social/friends.h) in the game's look: a list screen laid out like the server list (faces, names, a status
// line, the buttons below), Add Friend like Direct Connection, toasts for friends coming online, requests and invites.
// Opened from the title screen and the pause menu. In a singleplayer world it also hosts: Invite and "Friends Can
// Join" open the world to LAN first (the friends' connections arrive through that port), like the game's Open to LAN.
#include "entity/entities.h"
#include "social/friends.h"
#include "social/nostr.h"
#include "ui/frontend.h"
#include "ui/modules.h"
#include "ui/toasts.h"

#include <bcrypt.h>

#include <cmath>

namespace ui {

namespace {

using social::Friends;
using social::FriendInfo;
using social::FriendsSnapshot;
using social::Presence;
using social::RequestInfo;

constexpr int kHeader = 40, kFooter = 60, kRowW = 305, kEntryH = 36, kSectionH = 20;
const char* kLanModes[] = {"survival", "creative", "adventure", "spectator"};

struct Row {
    enum Kind { Section, Friend, Incoming, Outgoing } kind = Section;
    int index = 0;  // into the snapshot's list of that kind
    float y = 0, h = 0;
    std::string text;  // sections
    u32 color = 0xFFFFFFFF;
};

struct FriendsUi {
    FriendsSnapshot snap;
    std::vector<Row> rows;
    std::string selected;  // friend code of the selected row
    float scroll = 0;
    Field code;            // Add Friend
    std::string addError;
    std::string status;    // feedback in the header for a few seconds
    u32 statusColor = 0xFF55FF55;
    double statusUntil = 0;
    bool canJoin = false;  // our world: friends may join without an invite
    std::string level;     // our world's name
    bool levelKnown = false;
    Screen back = Screen::Title;
    int refreshTick = 0;
} g;

void setStatus(UiState& s, std::string text, u32 color = 0xFF55FF55) {
    g.status = std::move(text), g.statusColor = color, g.statusUntil = s.clock + 4.0;
}

const std::string& levelName(UiState& s) {
    FrontEndState& f = *s.fe;
    if (!g.levelKnown) {
        WorldEntry w;
        g.level = !f.cfg.worldName.empty() ? f.cfg.worldName : readWorld(f.cfg.worldDir, w) ? w.name : utf8(f.cfg.worldDir.filename());
        g.levelKnown = true;
    }
    return g.level;
}

// The skin an offline-mode server gives this name (DefaultPlayerSkin of the name-based UUID).
const Sprite* skinFor(UiState& s, const std::string& name) {
    const std::string seed = "OfflinePlayer:" + name;
    u8 uuid[16] = {};
    BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, PUCHAR(seed.data()), ULONG(seed.size()), uuid, 16);
    uuid[6] = u8((uuid[6] & 0x0F) | 0x30);
    uuid[8] = u8((uuid[8] & 0x3F) | 0x80);
    static const char* kSkins[9] = {"alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};
    const int i = ent::defaultSkinIndex(uuid);
    const Sprite* sp = s.sprites.texture(std::string("entity/player/") + (i < 9 ? "slim/" : "wide/") + kSkins[i % 9]);
    return sp ? sp : s.sprites.texture("entity/player/wide/steve");
}

void drawFace(UiState& s, Canvas& c, const std::string& name, float x, float y, float size, bool dim) {
    const Sprite* skin = skinFor(s, name);
    if (!skin) return;
    const u32 tint = dim ? 0xFF707070 : 0xFFFFFFFF;
    c.push();
    c.translate(x, y);
    c.scaleBy(size / 8.0f);
    blitTexture(c, skin, 0, 0, 8, 8, 8, 8, 64, 64, tint);   // face
    blitTexture(c, skin, 0, 0, 40, 8, 8, 8, 64, 64, tint);  // hat layer
    c.pop();
}

std::string ago(i64 unix) {
    if (unix <= 0) return "Offline";
    const i64 d = std::max<i64>(0, social::unixNow() - unix);
    auto n = [](i64 v, const char* unit) { return std::to_string(v) + " " + unit + (v == 1 ? "" : "s") + " ago"; };
    return "Last seen " + (d < 120 ? std::string("just now") : d < 7200 ? n(d / 60, "minute") : d < 172800 ? n(d / 3600, "hour") : n(d / 86400, "day"));
}

void refresh(UiState& s) {
    g.snap = Friends::get().snapshot();
    g.rows.clear();
    float y = 0;
    auto section = [&](std::string text, u32 color = 0xFFFFFFFF) {
        Row r;
        r.kind = Row::Section, r.y = y, r.h = kSectionH, r.text = std::move(text), r.color = color;
        g.rows.push_back(r);
        y += kSectionH;
    };
    auto entry = [&](Row::Kind k, int i) {
        Row r;
        r.kind = k, r.index = i, r.y = y, r.h = kEntryH;
        g.rows.push_back(r);
        y += kEntryH;
    };
    const bool requests = !g.snap.incoming.empty() || !g.snap.outgoing.empty();
    if (g.snap.started && g.snap.relaysConnected == 0) section("Connecting to the friends network...", 0xFFA0A0A0);
    if (!g.snap.incoming.empty()) {
        section("Friend Requests");
        for (int i = 0; i < int(g.snap.incoming.size()); ++i) entry(Row::Incoming, i);
    }
    if (requests && !g.snap.friends.empty()) {
        int online = 0;
        for (const FriendInfo& f : g.snap.friends) online += f.presence != Presence::Offline;
        section("Friends (" + std::to_string(online) + " online)");
    }
    for (int i = 0; i < int(g.snap.friends.size()); ++i) entry(Row::Friend, i);
    if (!g.snap.outgoing.empty()) {
        section("Sent Requests");
        for (int i = 0; i < int(g.snap.outgoing.size()); ++i) entry(Row::Outgoing, i);
    }
    (void)s;
}

std::string rowCode(const Row& r) {
    switch (r.kind) {
        case Row::Friend: return g.snap.friends[size_t(r.index)].code;
        case Row::Incoming: return g.snap.incoming[size_t(r.index)].code;
        case Row::Outgoing: return g.snap.outgoing[size_t(r.index)].code;
        default: return {};
    }
}

const Row* selectedRow() {
    for (const Row& r : g.rows)
        if (r.kind != Row::Section && rowCode(r) == g.selected) return &r;
    return nullptr;
}
const FriendInfo* selectedFriend() {
    const Row* r = selectedRow();
    return r && r->kind == Row::Friend ? &g.snap.friends[size_t(r->index)] : nullptr;
}
bool canJoinFriend(const FriendInfo& f) { return f.presence != Presence::Offline && (f.joinable || f.invitedMe) && !f.hosting; }

float listHeight() {
    return g.rows.empty() ? 0 : g.rows.back().y + g.rows.back().h + 4;
}
float maxScroll(UiState& s) { return std::max(0.0f, listHeight() - float(s.guiH() - kHeader - kFooter)); }

// Open to LAN for friends (the game's ShareToLanScreen defaults: the host's game mode, the world's cheats setting).
bool ensureHosting(UiState& s) {
    FrontEndState& f = *s.fe;
    if (!f.cfg.singleplayer) return false;
    if (f.lanOpenPort == 0) {
        WorldEntry w;
        const bool have = readWorld(f.cfg.worldDir, w);
        const u16 port = net::suggestLanPort();
        std::string error;
        if (!f.cfg.openToLan ||
            !f.cfg.openToLan(f.cfg.playerName + " - " + levelName(s), kLanModes[std::clamp(s.gameMode(), 0, 3)], have && w.cheats, port, error)) {
            setStatus(s, "Could not open the world to friends" + (error.empty() ? std::string() : ": " + error), 0xFFFF5555);
            return false;
        }
        f.lanOpenPort = port;
        s.addChat(translatable("commands.publish.started", {Text(std::to_string(port))}, s.lang()));
    }
    Friends::get().setHost(f.lanOpenPort, g.canJoin);
    return true;
}

void join(UiState& s, const FriendInfo& fr) {
    FrontEndState& f = *s.fe;
    if (!f.cfg.menu) {  // leave this world (saved like Save and Quit), then join from the title screen
        requestTitle(s, "friend-join:" + fr.code);
        return;
    }
    std::vector<std::wstring> a = launch::stickyArguments();
    a.insert(a.end(), {L"--friend-join", widen(fr.code), L"--name", widen(f.cfg.playerName)});
    LOG("ui: joining %s's world", fr.name.c_str());
    f.failTitle = Text("Couldn't join " + fr.name + "'s world");
    startChild(s, a, Text("Joining " + fr.name + "'s world"), Screen::Friends, true);
}

void invite(UiState& s, const FriendInfo& fr) {
    if (!ensureHosting(s)) return;
    if (Friends::get().invite(fr.pub)) setStatus(s, "Invited " + fr.name + " to " + levelName(s));
    else setStatus(s, "Could not invite " + fr.name, 0xFFFF5555);
    s.buildButtons();
}

void removeSelected(UiState& s) {
    const Row* r = selectedRow();
    if (!r) return;
    Friends& fr = Friends::get();
    if (r->kind == Row::Incoming) {
        const RequestInfo q = g.snap.incoming[size_t(r->index)];
        fr.decline(q.pub);
        setStatus(s, "Declined " + q.name, 0xFFA0A0A0);
    } else if (r->kind == Row::Outgoing) {
        fr.cancel(g.snap.outgoing[size_t(r->index)].pub);
        setStatus(s, "Friend request cancelled", 0xFFA0A0A0);
    } else if (r->kind == Row::Friend) {
        const FriendInfo f = g.snap.friends[size_t(r->index)];
        showConfirm(s, Text("Remove " + f.name + " from your friends?"),
                    Text("You will no longer see each other online or join each other's worlds."), "Remove",
                    [p = &s, f] {
                        Friends::get().remove(f.pub);
                        setStatus(*p, "Removed " + f.name, 0xFFA0A0A0);
                        g.selected.clear();
                        p->openScreen(Screen::Friends);
                    },
                    Screen::Friends);
        return;
    }
    g.selected.clear();
    refresh(s);
    s.buildButtons();
}

void acceptSelected(UiState& s) {
    const Row* r = selectedRow();
    if (!r || r->kind != Row::Incoming) return;
    const RequestInfo q = g.snap.incoming[size_t(r->index)];
    Friends::get().accept(q.pub);
    setStatus(s, "You and " + q.name + " are now friends");
    refresh(s);
    s.buildButtons();
}

void goBack(UiState& s) {
    if (g.back == Screen::None && !s.fe->cfg.menu) return s.closeScreen();  // opened over the game (the friends key)
    s.openScreen(g.back == Screen::Pause && !s.fe->cfg.menu ? Screen::Pause : Screen::Title);
}

// --- Friends screen -----------------------------------------------------------------------------------------------

void build(UiState& s) {
    FrontEndState& f = *s.fe;
    setFields(s, {}, -1);
    f.onBack = nullptr, f.onEnter = nullptr;
    refresh(s);
    const int W = s.guiW(), H = s.guiH();
    UiState* p = &s;
    const float x0 = float(W / 2 - 154), y0 = float(H - 52), y1 = float(H - 28);
    const FriendInfo* fr = selectedFriend();
    const Row* row = selectedRow();
    const bool host = f.cfg.singleplayer && !f.cfg.menu;
    // Join, or Invite while hosting (Join again when that friend has a world open for us).
    if (host && !(fr && canJoinFriend(*fr))) {
        addButton(s, x0, y0, 100, Text("Invite"), [p] {
            if (const FriendInfo* x = selectedFriend()) invite(*p, *x);
        }, fr && fr->presence != Presence::Offline && Friends::get().started());
    } else {
        addButton(s, x0, y0, 100, Text("Join"), [p] {
            if (const FriendInfo* x = selectedFriend()) join(*p, *x);
        }, fr && canJoinFriend(*fr));
    }
    addButton(s, x0 + 104, y0, 100, Text("Add Friend"), [p] {
        g.code.text = TextField{};
        g.code.text.maxLength = 80;
        g.addError.clear();
        p->openScreen(Screen::AddFriend);
    }, Friends::get().started());
    const char* removeLabel = row && row->kind == Row::Incoming ? "Decline" : row && row->kind == Row::Outgoing ? "Cancel" : "Remove";
    addButton(s, x0 + 208, y0, 100, Text(removeLabel), [p] { removeSelected(*p); }, row != nullptr);
    addButton(s, x0, y1, 74, Text("Accept"), [p] { acceptSelected(*p); }, row && row->kind == Row::Incoming);
    if (host) {
        Button& b = addButton(s, x0 + 78, y1, 152, Text(), [p] {
            g.canJoin = !g.canJoin;
            if (g.canJoin) {
                if (ensureHosting(*p)) setStatus(*p, "Friends can join " + levelName(*p) + " now");
                else g.canJoin = false;
            } else {
                Friends::get().setHost(p->fe->lanOpenPort, false);
                setStatus(*p, "Only invited friends can join", 0xFFA0A0A0);
            }
        }, Friends::get().started());
        b.labelFn = [p] {
            return trf(*p, "options.generic_value", {Text("Friends Can Join"), tr(*p, g.canJoin ? "options.on" : "options.off")});
        };
    } else {
        addButton(s, x0 + 78, y1, 152, Text("Copy My Code"), [p] {
            p->window->setClipboard(g.snap.myCode);
            setStatus(*p, "Friend code copied to the clipboard");
        }, !g.snap.myCode.empty());
    }
    addButton(s, x0 + 234, y1, 74, tr(s, "gui.back"), [p] { goBack(*p); });
    f.onBack = [p] { goBack(*p); };
    f.onEnter = [p] {
        const Row* r = selectedRow();
        if (r && r->kind == Row::Incoming) acceptSelected(*p);
        else if (const FriendInfo* x = selectedFriend(); x && canJoinFriend(*x)) join(*p, *x);
    };
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    if (s.guiTicks - g.refreshTick >= 10) {  // twice a second: presence changes, and the buttons that depend on it
        g.refreshTick = s.guiTicks;
        s.buildButtons();
    }
    menuBackground(s, c);
    s.drawCentered(c, Text("Friends"), float(W / 2), 12, 0xFFFFFFFF);
    // Header: our friend code (click to copy), or the last action's result for a few seconds.
    const float codeY = 26;
    bool codeHover = false;
    if (s.clock < g.statusUntil) {
        s.drawCentered(c, Text(g.status), float(W / 2), codeY, g.statusColor);
    } else if (!g.snap.started) {
        s.drawCentered(c, Text("Friends are off in this run"), float(W / 2), codeY, 0xFFA0A0A0);
    } else {
        Text t("Your friend code: ", colorStyle(0xA0A0A0));
        t.append(g.snap.myCode, colorStyle(0xFFFFFF));
        const int tw = s.font.width(t);
        const float tx = float(W / 2 - tw / 2);
        codeHover = s.mouseX >= tx && s.mouseX < tx + float(tw) && s.mouseY >= codeY - 1 && s.mouseY < codeY + 9;
        s.drawText(c, t, tx, codeY, 0xFFFFFFFF, true);
        if (codeHover) c.fill(tx + float(s.font.width(Text("Your friend code: "))), codeY + 9, tx + float(tw), codeY + 10, 0xFFFFFFFF);
    }
    const float top = float(kHeader), bottom = float(H - kFooter);
    listBackground(s, c, top, bottom);
    const float rowLeft = float(W / 2 - kRowW / 2 + 2), right = rowLeft + kRowW - 6;
    g.scroll = std::clamp(g.scroll, 0.0f, maxScroll(s));
    std::vector<Text> tooltip;
    if (codeHover) tooltip = {Text("Click to copy. Friends add you with this code.")};
    c.pushScissor(0, top, float(W), bottom);
    if (g.rows.empty() || (g.rows.size() == 1 && g.rows[0].kind == Row::Section)) {
        const float my = top + (bottom - top) / 2 - 10 + (g.rows.empty() ? 0 : 10);
        s.drawCentered(c, Text(g.snap.started ? "No friends yet" : "Friends need the game to be started normally"),
                       float(W / 2), my, 0xFFFFFFFF);
        if (g.snap.started)
            s.drawCentered(c, Text("Share your friend code, or add a friend with theirs."), float(W / 2), my + 12, 0xFF808080);
    }
    for (const Row& r : g.rows) {
        const float y = top + 4 + r.y - g.scroll;
        if (y + r.h < top || y > bottom) continue;
        if (r.kind == Row::Section) {
            s.drawCentered(c, Text(r.text), float(W / 2), y + 6, r.color);
            continue;
        }
        const bool hovered = s.mouseX >= rowLeft - 2 && s.mouseX < rowLeft + kRowW - 2 && s.mouseY >= y - 2 &&
                             s.mouseY < y + r.h - 2 && s.mouseY >= top && s.mouseY < bottom;
        if (rowCode(r) == g.selected) {
            c.fill(rowLeft - 2, y - 2, rowLeft + kRowW - 2, y + r.h - 2, 0xFFFFFFFF);
            c.fill(rowLeft - 1, y - 1, rowLeft + kRowW - 3, y + r.h - 3, 0xFF000000);
        }
        std::string name, line2, line3, right1;
        u32 line2Color = 0xFF808080, line3Color = 0xFF808080, right1Color = 0xFF808080;
        bool dim = false, joinable = false;
        if (r.kind == Row::Friend) {
            const FriendInfo& f = g.snap.friends[size_t(r.index)];
            name = f.name.empty() ? "?" : f.name;
            dim = f.presence == Presence::Offline;
            joinable = canJoinFriend(f);
            switch (f.presence) {
                case Presence::Offline: line2 = ago(f.lastSeen), right1 = "Offline", right1Color = 0xFF808080; break;
                case Presence::Online: line2 = "In the menus", right1 = "Online", right1Color = 0xFF55FF55; break;
                case Presence::Multiplayer:
                    line2 = f.world.empty() ? "Playing multiplayer" : f.world == g.snap.myName ? "Playing in your world" : "Playing with " + f.world;
                    right1 = "Online", right1Color = 0xFF55FF55;
                    break;
                case Presence::InWorld: line2 = "Playing \"" + f.world + "\"", right1 = "Online", right1Color = 0xFF55FF55; break;
            }
            if (f.hosting) line3 = "You are in this world", line3Color = 0xFF55FF55;
            else if (f.ourGuest && f.presence != Presence::Multiplayer) line3 = "In your world now", line3Color = 0xFF55FF55;
            else if (f.invitedMe) line3 = "Invited you to join", line3Color = 0xFFFFFF55;
            else if (f.joinable) line3 = "Open to friends", line3Color = 0xFF55FF55;
            else if (f.invitedByMe) line3 = "Invited to your world", line3Color = 0xFF808080;
        } else if (r.kind == Row::Incoming) {
            const RequestInfo& q = g.snap.incoming[size_t(r.index)];
            name = q.name.empty() ? "?" : q.name;
            line2 = "Wants to be your friend", line2Color = 0xFFFFFF55;
            line3 = "Accept or Decline below";
            right1 = "Request", right1Color = 0xFFFFFF55;
        } else {
            const RequestInfo& q = g.snap.outgoing[size_t(r.index)];
            name = q.name.empty() ? "Friend request sent" : q.name;
            line2 = "Waiting for them to accept";
            line3 = fitText(s.font, q.code, kRowW - 40);
            line3Color = 0xFF505050;
            right1 = "Pending";
        }
        drawFace(s, c, r.kind == Row::Outgoing && name == "Friend request sent" ? std::string("?") : name, rowLeft, y, 32, dim);
        s.drawText(c, Text(fitText(s.font, name, kRowW - 110)), rowLeft + 35, y + 1, dim ? 0xFFA0A0A0 : 0xFFFFFFFF, true);
        s.drawText(c, Text(fitText(s.font, line2, kRowW - 40)), rowLeft + 35, y + 12, line2Color, true);
        if (!line3.empty()) s.drawText(c, Text(fitText(s.font, line3, kRowW - 40)), rowLeft + 35, y + 23, line3Color, true);
        if (!right1.empty()) s.drawText(c, Text(right1), right - float(s.font.width(Text(right1))), y + 1, right1Color, true);
        if (hovered && joinable) {  // the server list's join arrow over the face
            c.fill(rowLeft, y, rowLeft + 32, y + 32, 0xA0909090);
            const bool over = s.mouseX >= rowLeft && s.mouseX < rowLeft + 32 && s.mouseY >= y && s.mouseY < y + 32;
            blitSprite(c, s.sprites.sprite(over ? "server_list/join_highlighted" : "server_list/join"), rowLeft, y, 32, 32);
            if (over) tooltip = {Text("Join " + name + "'s world")};
        }
    }
    c.popScissor();
    if (const float ms = maxScroll(s); ms > 0) {
        const float x = rowLeft + kRowW + 4, h = bottom - top;
        const float bar = std::max(32.0f, h * h / (h + ms)), by = top + (h - bar) * g.scroll / ms;
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), x, top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), x, by, 6, bar);
        s.scrollbarCursor(x, top, h);
    }
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
}

void input(UiState& s, const InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *s.fe;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return goBack(s);
    if (e.type == E::Key && e.down && e.code == VK_RETURN) {
        if (auto fn = f.onEnter) fn();
        return;
    }
    const int W = s.guiW(), H = s.guiH();
    const float top = float(kHeader), bottom = float(H - kFooter), rowLeft = float(W / 2 - kRowW / 2 + 2);
    if (e.type == E::Wheel) {
        g.scroll = std::clamp(g.scroll - float(e.code) * kEntryH / 2, 0.0f, maxScroll(s));
        return;
    }
    if (e.type == E::Key && e.down && (e.code == VK_UP || e.code == VK_DOWN)) {
        int at = -1;
        for (int i = 0; i < int(g.rows.size()); ++i)
            if (g.rows[size_t(i)].kind != Row::Section && rowCode(g.rows[size_t(i)]) == g.selected) at = i;
        int i = at;
        do i += e.code == VK_DOWN ? 1 : -1;
        while (i >= 0 && i < int(g.rows.size()) && g.rows[size_t(i)].kind == Row::Section);
        if (i >= 0 && i < int(g.rows.size())) {
            const Row& r = g.rows[size_t(i)];
            g.selected = rowCode(r);
            g.scroll = std::clamp(g.scroll, r.y + r.h + 4 - (bottom - top), r.y);
            s.buildButtons();
        }
        return;
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.mouseY >= 22 && s.mouseY < 36 && s.clock >= g.statusUntil &&
        !g.snap.myCode.empty()) {
        const int tw = s.font.width(Text("Your friend code: " + g.snap.myCode));
        if (std::abs(s.mouseX - float(W / 2)) < float(tw / 2)) {
            s.window->setClipboard(g.snap.myCode);
            setStatus(s, "Friend code copied to the clipboard");
            return;
        }
    }
    if (e.type == E::Button && e.down && e.code == 0 && s.mouseY >= top && s.mouseY < bottom && s.mouseX >= rowLeft - 2 &&
        s.mouseX < rowLeft + kRowW - 2) {
        const float ly = s.mouseY - top - 4 + 2 + g.scroll;
        for (int i = 0; i < int(g.rows.size()); ++i) {
            const Row& r = g.rows[size_t(i)];
            if (r.kind == Row::Section || ly < r.y || ly >= r.y + r.h) continue;
            g.selected = rowCode(r);
            s.buildButtons();
            const float y = top + 4 + r.y - g.scroll;
            const float rx = s.mouseX - rowLeft, ry = s.mouseY - y;
            const bool dbl = doubleClicked(s, i);
            if (r.kind == Row::Friend) {
                const FriendInfo& fr = g.snap.friends[size_t(r.index)];
                if (canJoinFriend(fr) && (dbl || (rx >= 0 && rx < 32 && ry >= 0 && ry < 32))) join(s, fr);
            } else if (r.kind == Row::Incoming && dbl) {
                acceptSelected(s);
            }
            return;
        }
        return;
    }
    s.buttonsInput(e);
}

void closed(UiState& s) { setFields(s, {}, -1); }

const ScreenRegistrar g_friendsScreen({Screen::Friends, build, draw, input, closed});

// --- Add Friend (like Direct Connection) --------------------------------------------------------------------------

void submitCode(UiState& s) {
    std::string code = g.code.text.value;
    if (code.empty()) return;
    switch (Friends::get().add(code)) {
        case Friends::AddResult::Sent:
            setStatus(s, "Friend request sent");
            s.openScreen(Screen::Friends);
            return;
        case Friends::AddResult::Accepted:
            setStatus(s, "You are now friends");
            s.openScreen(Screen::Friends);
            return;
        case Friends::AddResult::AlreadyFriends: g.addError = "You are already friends."; return;
        case Friends::AddResult::Self: g.addError = "That is your own friend code."; return;
        case Friends::AddResult::NotStarted: g.addError = "Friends are not available right now."; return;
        case Friends::AddResult::Invalid: g.addError = "That is not a friend code. Check it for typos."; return;
    }
}

void buildAdd(UiState& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    UiState* p = &s;
    const float x = float(W / 2 - 100);
    g.code.x = x, g.code.y = 116, g.code.w = 200;
    g.code.hint = "Paste a friend code";
    setFields(s, {&g.code}, 0);
    Button& add = addButton(s, x, float(H / 4 + 96 + 12), 200, Text("Send Friend Request"), [p] { submitCode(*p); });
    add.labelFn = [] { return Text("Send Friend Request"); };
    addButton(s, x, float(H / 4 + 120 + 12), 200, tr(s, "gui.cancel"), [p] { p->openScreen(Screen::Friends); });
    f.onBack = [p] { p->openScreen(Screen::Friends); };
    f.onEnter = [p] { submitCode(*p); };
}

void drawAdd(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, Text("Add Friend"), float(W / 2), 20, 0xFFFFFFFF);
    s.drawText(c, Text("Friend Code"), float(W / 2 - 100), 100, 0xFFA0A0A0, true);
    for (Button& b : s.buttons)
        if (b.labelFn) b.active = !g.code.text.value.empty();
    drawFields(s, c);
    float y = 142;
    const std::string msg = !g.addError.empty() ? g.addError : "Ask your friend to copy theirs from Friends.";
    for (const Text& l : s.font.split(Text(msg), 300)) {
        s.drawCentered(c, l, float(W / 2), y, g.addError.empty() ? 0xFF808080 : 0xFFFF5555);
        y += 10;
    }
    s.drawButtons(c);
}

void inputAdd(UiState& s, const InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *s.fe;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return s.openScreen(Screen::Friends);
    if (e.type == E::Key && e.down && e.code == VK_RETURN) {
        if (auto fn = f.onEnter) fn();
        return;
    }
    if (fieldsInput(s, e)) {
        g.addError.clear();
        return;
    }
    s.buttonsInput(e);
}

const ScreenRegistrar g_addScreen({Screen::AddFriend, buildAdd, drawAdd, inputAdd, closed});

// --- presence, hosting, toasts ------------------------------------------------------------------------------------

void tick(UiState& s) {
    Friends& fr = Friends::get();
    if (!fr.started() || !s.fe) return;
    if (s.guiTicks % 20 == 0) {  // what we are doing, and whether our world takes friends
        FrontEndState& f = *s.fe;
        if (f.cfg.menu) {
            fr.setActivity(Presence::Online, {});
        } else if (f.cfg.singleplayer) {
            fr.setActivity(Presence::InWorld, levelName(s));
        } else {  // a server, or a friend's world (whose: shown to our friends)
            std::string with;
            for (const FriendInfo& x : fr.snapshot().friends)
                if (x.hosting) with = x.name;
            fr.setActivity(Presence::Multiplayer, with);
        }
        if (f.cfg.singleplayer && !f.cfg.menu && f.lanOpenPort) fr.setHost(f.lanOpenPort, g.canJoin);
        else fr.setHost(0, false);
    }
    for (const social::FriendEvent& e : fr.takeEvents()) {
        using T = social::FriendEvent::Type;
        switch (e.type) {
            case T::Online: showToast(Text(e.name + " is online")); break;
            case T::Request: showToast(Text("Friend Request"), Text(e.name + " wants to be friends")); break;
            case T::Accepted: showToast(Text("Friend Request Accepted"), Text(e.name + " is now your friend")); break;
            case T::Invited:
                showToast(Text(e.name + " invited you"), Text(s.fe->cfg.menu ? "Join from the Friends screen" : "Open Friends to join"));
                break;
        }
    }
}

const UiModuleRegistrar g_module({.name = "friends", .tick = tick});

}  // namespace

// --- entry points (title screen, pause menu) ----------------------------------------------------------------------

void openFriendsScreen(UiState& s) {
    g.back = s.fe && !s.fe->cfg.menu ? Screen::Pause : Screen::Title;
    g.scroll = 0;
    s.openScreen(Screen::Friends);
}

// Minecraft.toggleFriendsScreen: the friends key opens Friends over the game, the title screen or the pause menu
// and closes it again; true when used.
bool toggleFriendsScreen(UiState& s) {
    if (s.screen == Screen::Friends) {
        goBack(s);
        return true;
    }
    if (s.screen != Screen::None && s.screen != Screen::Title && s.screen != Screen::Pause) return false;
    g.back = s.fe->cfg.menu ? Screen::Title : s.screen;
    g.scroll = 0;
    s.openScreen(Screen::Friends);
    return true;
}

int friendsPendingRequests() { return Friends::get().started() ? Friends::get().pendingRequests() : 0; }

Text friendsButtonLabel() {
    const int requests = Friends::get().started() ? Friends::get().pendingRequests() : 0;
    return Text(requests ? "Friends (" + std::to_string(requests) + ")" : "Friends");
}

// The title screen opened with --menu friend-join:<code> (a world left to join a friend): join right away.
bool friendsMenuCommand(UiState& s, const std::string& command) {
    if (!command.starts_with("friend-join:")) return false;
    openFriendsScreen(s);
    social::Key32 key;
    const std::string code = command.substr(12);
    if (!Friends::parseCode(code, key)) return true;
    FriendInfo f;
    f.pub = key, f.code = code, f.name = "your friend";
    refresh(s);
    for (const FriendInfo& x : g.snap.friends)
        if (x.pub == key) f.name = x.name;
    join(s, f);
    return true;
}

}  // namespace ui
