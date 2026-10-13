// Spectator hotbar menu (SpectatorGui / SpectatorMenu): the hotbar keys open a menu of commands (teleport to a
// player, to a team member) shown in the hotbar; a key selects an item, the same key again uses it. The wheel moves
// the selection (without the menu it changes the flying speed); the Spectator Hotbar key (middle button) uses the
// selected item or opens the menu.
#include "game/game.h"
#include "net/client.h"
#include "net/protocol.h"
#include "ui/canvas.h"
#include "ui/modules.h"
#include "ui/mp_hud.h"
#include "ui/sprites.h"
#include "ui/ui_state.h"

#include <algorithm>
#include <cmath>

namespace ui {
namespace {

enum class Kind : u8 { Empty, Close, ScrollLeft, ScrollRight, ScrollRightDisabled, PlayerCategory, TeamCategory, Player, Team };
struct Item {
    Kind kind = Kind::Empty;
    PlayerInfoView player;              // Player; Team: the member whose face it shows
    Text teamName;                      // Team
    int teamColor = -1;
    std::vector<PlayerInfoView> members;  // Team (non-spectators), PlayerCategory (the players listed)
    std::vector<Item> teams;              // TeamCategory: its teams, built with the root when the menu opens
};
struct Category {
    const char* prompt;
    std::vector<Item> items;
};

struct State {
    bool active = false;
    Category category;
    int selected = -1, page = 0;
    double lastSelection = 0;  // seconds (Util.getMillis)
} g;

// Comparator.comparing(profile id): UUID.compareTo, the most significant half first, both signed.
bool uuidLess(const PlayerInfoView& a, const PlayerInfoView& b) {
    auto half = [](const std::string& u, int at) {
        i64 v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | i64(u8(size_t(at + i) < u.size() ? u[size_t(at + i)] : 0));
        return v;
    };
    const i64 am = half(a.uuid, 0), bm = half(b.uuid, 0);
    return am != bm ? am < bm : half(a.uuid, 8) < half(b.uuid, 8);
}

Category playersCategory(std::vector<PlayerInfoView> players) {  // TeleportToPlayerMenuCategory
    std::erase_if(players, [](const PlayerInfoView& p) { return p.gameMode == 3; });
    std::sort(players.begin(), players.end(), uuidLess);
    Category c{"spectatorMenu.teleport.prompt", {}};
    for (PlayerInfoView& p : players) {
        Item i{Kind::Player};
        i.player = std::move(p);
        c.items.push_back(std::move(i));
    }
    return c;
}

std::vector<PlayerInfoView> listedPlayers() {
    std::vector<PlayerInfoView> v = playerInfos();
    std::erase_if(v, [](const PlayerInfoView& p) { return !p.listed; });
    return v;
}

Category teamsCategory();

Category rootCategory() {  // RootSpectatorMenuCategory: built when the menu opens
    Category c{"spectatorMenu.root.prompt", {}};
    Item players{Kind::PlayerCategory};
    players.members = listedPlayers();
    std::erase_if(players.members, [](const PlayerInfoView& p) { return p.gameMode == 3; });
    c.items.push_back(std::move(players));
    Item teams{Kind::TeamCategory};
    teams.teams = teamsCategory().items;  // TeleportToTeamMenuCategory: its teams as the menu opens
    c.items.push_back(std::move(teams));
    return c;
}

Category teamsCategory() {  // TeleportToTeamMenuCategory
    Category c{"spectatorMenu.team_teleport.prompt", {}};
    static u64 pick = 0x9E3779B97F4A7C15ULL;
    for (const TeamView& t : teamViews()) {
        Item i{Kind::Team};
        for (const std::string& name : t.players)
            for (const PlayerInfoView& p : playerInfos())
                if (p.name == name && p.gameMode != 3) i.members.push_back(p);
        if (i.members.empty()) continue;
        pick = pick * 6364136223846793005ULL + 1442695040888963407ULL;  // a random member's face
        i.player = i.members[size_t((pick >> 33) % i.members.size())];
        i.teamName = t.displayName, i.teamColor = t.color;
        c.items.push_back(std::move(i));
    }
    return c;
}

bool enabled(const Item& i) {
    switch (i.kind) {
        case Kind::Empty:
        case Kind::ScrollRightDisabled: return false;
        case Kind::PlayerCategory: return !i.members.empty();
        case Kind::TeamCategory: return !i.teams.empty();  // TeleportToTeamMenuCategory.isEnabled
        case Kind::Player: {  // the player's game mode now
            PlayerInfoView now;
            return findPlayerInfo(i.player.uuid, now) && now.gameMode != 3;
        }
        default: return true;
    }
}

Item itemAt(int slot) {  // SpectatorMenu.getItem
    const int index = slot + g.page * 6;
    const std::vector<Item>& items = g.category.items;
    if (g.page > 0 && slot == 0) return Item{Kind::ScrollLeft};
    if (slot == 7) return Item{index < int(items.size()) ? Kind::ScrollRight : Kind::ScrollRightDisabled};
    if (slot == 8) return Item{Kind::Close};
    return index >= 0 && index < int(items.size()) ? items[size_t(index)] : Item{};
}

Text itemName(UiState& s, const Item& i) {
    switch (i.kind) {
        case Kind::Close: return Text(s.res.translate("spectatorMenu.close"));
        case Kind::ScrollLeft: return Text(s.res.translate("spectatorMenu.previous_page"));
        case Kind::ScrollRight:
        case Kind::ScrollRightDisabled: return Text(s.res.translate("spectatorMenu.next_page"));
        case Kind::PlayerCategory: return Text(s.res.translate("spectatorMenu.teleport"));
        case Kind::TeamCategory: return Text(s.res.translate("spectatorMenu.team_teleport"));
        case Kind::Player: return Text(i.player.name);
        case Kind::Team: return i.teamName;
        default: return Text();
    }
}

void open(UiState& s) {
    g.active = true;
    g.category = rootCategory();
    g.selected = -1, g.page = 0;
    (void)s;
}

void exitMenu() {  // SpectatorMenu.exit -> onSpectatorMenuClosed
    g.active = false;
    g.lastSelection = 0;
}

void selectCategory(Category c) {
    g.category = std::move(c);
    g.selected = -1, g.page = 0;
}

void use(UiState& s, const Item& i) {  // SpectatorMenuItem.selectItem
    switch (i.kind) {
        case Kind::Close: exitMenu(); break;
        case Kind::ScrollLeft: --g.page; break;
        case Kind::ScrollRight: ++g.page; break;
        case Kind::PlayerCategory: selectCategory(playersCategory(i.members)); break;
        case Kind::TeamCategory: selectCategory(Category{"spectatorMenu.team_teleport.prompt", i.teams}); break;
        case Kind::Team: selectCategory(playersCategory(i.members)); break;
        case Kind::Player:
            if (s.client && i.player.uuid.size() == 16) {  // ServerboundTeleportToEntityPacket
                net::Writer w;
                w.bytes(i.player.uuid.data(), 16);
                s.client->sendPlay("teleport_to_entity", w);
            }
            break;
        default: break;
    }
}

void selectSlot(UiState& s, int slot) {  // SpectatorMenu.selectSlot
    const Item i = itemAt(slot);
    if (i.kind == Kind::Empty) return;
    if (g.selected == slot && enabled(i)) use(s, i);
    else g.selected = slot;
}

float hotbarAlpha(const UiState& s) {  // (lastSelectionTime - now + 5000) / 2000
    return std::clamp(float((g.lastSelection - s.clock + 5.0) / 2.0), 0.0f, 1.0f);
}

bool input(UiState& s, int what, int value) {
    if (!s.spectator()) return false;
    if (what == 0) {  // SpectatorGui.onHotbarSelected
        g.lastSelection = s.clock;
        if (g.active) selectSlot(s, value);
        else open(s);
        return true;
    }
    if (what == 1) {  // MouseHandler.onScroll
        if (!g.active) {  // the flying speed
            float& speed = s.game->player().flyingSpeed;
            speed = std::clamp(speed + float(value) * 0.005f, 0.0f, 0.2f);
            return true;
        }
        int slot = g.selected - value;  // onMouseScrolled(-wheel)
        const int step = -value > 0 ? 1 : -1;
        while (slot >= 0 && slot <= 8 && !enabled(itemAt(slot))) slot += step;
        if (slot >= 0 && slot <= 8) {
            selectSlot(s, slot);
            g.lastSelection = s.clock;
        }
        return true;
    }
    return false;
}

void attached(UiState& s) {
    g = State{};
    if (s.game) s.game->spectatorInput = [&s](int what, int value) { return input(s, what, value); };
}

bool worldKey(UiState& s, const InputEvent& e) {  // SpectatorGui.onHotbarActionKeyPressed (the Spectator Hotbar key)
    if (!e.repeat && s.isKey(Key::ToggleSpectatorShaderEffects, e)) {  // Minecraft.handleKeybinds (any game mode)
        ++s.spectatorEffectToggles;
        return true;
    }
    if (!s.spectator() || e.repeat || !s.isKey(Key::SpectatorHotbar, e)) return false;
    g.lastSelection = s.clock;
    if (!g.active) open(s);
    else if (g.selected != -1) selectSlot(s, g.selected);
    return false;  // the pick block key may share the button
}

void tick(UiState& s) {
    if (g.active && !s.spectator()) exitMenu();
}

void drawHud(UiState& s, Canvas& c) {
    if (!s.spectator() || !g.active) return;
    const float alpha = hotbarAlpha(s);
    if (alpha <= 0) return exitMenu();
    const int gw = c.guiWidth(), gh = c.guiHeight(), cx = gw / 2;
    const float y = std::floor(float(gh) - 22.0f * alpha);
    const u32 white = u32(alpha * 255.0f) << 24 | 0xFFFFFF;
    blitSprite(c, s.sprites.sprite("hud/hotbar"), float(cx - 91), y, 182, 22, white);
    if (g.selected >= 0) blitSprite(c, s.sprites.sprite("hud/hotbar_selection"), float(cx - 91 - 1 + g.selected * 20), y - 1, 24, 23, white);
    for (int slot = 0; slot < 9; ++slot) {
        const Item i = itemAt(slot);
        if (i.kind == Kind::Empty) continue;
        const float x = float(gw / 2 - 90 + slot * 20 + 2), iy = y + 3;
        const bool on = enabled(i);
        const float bright = on ? 1.0f : 0.25f;
        const u32 shade = u32(alpha * 255.0f) << 24 | u32(bright * 255.0f) * 0x010101u;
        auto face = [&](const PlayerInfoView& p, u32 color) {  // PlayerFaceExtractor at (2, 2), 12 pixels
            const Sprite* skin = s.sprites.texture(p.skin);
            c.push();
            c.translate(x + 2, iy + 2);
            c.scaleBy(1.5f);
            blitTexture(c, skin, 0, 0, 8, 8, 8, 8, 64, 64, color);
            blitTexture(c, skin, 0, 0, 40, 8, 8, 8, 64, 64, color);  // PlayerFaceExtractor: the hat layer always
            c.pop();
        };
        switch (i.kind) {
            case Kind::Close: blitSprite(c, s.sprites.sprite("spectator/close"), x, iy, 16, 16, shade); break;
            case Kind::ScrollLeft: blitSprite(c, s.sprites.sprite("spectator/scroll_left"), x, iy, 16, 16, shade); break;
            case Kind::ScrollRight:
            case Kind::ScrollRightDisabled: blitSprite(c, s.sprites.sprite("spectator/scroll_right"), x, iy, 16, 16, shade); break;
            case Kind::PlayerCategory: blitSprite(c, s.sprites.sprite("spectator/teleport_to_player"), x, iy, 16, 16, shade); break;
            case Kind::TeamCategory: blitSprite(c, s.sprites.sprite("spectator/teleport_to_team"), x, iy, 16, 16, shade); break;
            case Kind::Player: face(i.player, u32(alpha * 255.0f) << 24 | 0xFFFFFF); break;  // ARGB.white(alpha)
            case Kind::Team:
                if (i.teamColor >= 0 && i.teamColor < 16) {
                    const u32 rgb = kLegacyColors[i.teamColor];
                    const u32 r = u32(float((rgb >> 16) & 255) * bright), gr = u32(float((rgb >> 8) & 255) * bright), b = u32(float(rgb & 255) * bright);
                    c.fill(x + 1, iy + 1, x + 15, iy + 15, 0xFF000000u | r << 16 | gr << 8 | b);
                }
                face(i.player, shade);
                break;
            default: break;
        }
        if (on) {  // the hotbar key's name at the slot's bottom right
            const Text key = s.keyLabel(s.settings.key(Key(int(Key::Hotbar1) + slot)));
            s.font.draw(c, key, x + 19 - 2 - float(s.font.width(key)), std::floor(iy) + 6 + 3, white, true);
        }
    }
    // SpectatorGui.extractAction: the selected item's name, or the category's prompt.
    const Item sel = g.selected >= 0 ? itemAt(g.selected) : Item{};
    const Text action = sel.kind == Kind::Empty ? Text(s.res.translate(g.category.prompt)) : itemName(s, sel);
    const int w = s.font.width(action);
    const float ax = float((gw - w) / 2), ay = float(gh - 35);
    const u32 bg = s.textBackground(0.0f);
    if (alphaOf(bg))
        c.fill(ax - 2, ay - 2, ax + float(w) + 2, ay + 11, withAlpha(0, float(alphaOf(bg)) * alpha / 255.0f));
    s.font.draw(c, action, ax, ay, white, true);
}

const UiModuleRegistrar kModule({.name = "spectator_menu", .attached = attached, .tick = tick, .drawHud = drawHud, .worldKey = worldKey});

}  // namespace
}  // namespace ui
