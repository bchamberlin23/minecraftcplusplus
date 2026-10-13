// World Options (WorldOptionsScreen, from the pause menu): the world's default and the host's own game mode, Allow
// Cheats, the difficulty and its lock, Edit Game Rules (InWorldGameRulesScreen: the server's values, sent back with
// ServerboundSetGameRulePacket) and Restrictions (RestrictionsScreen: what the chat lets you do); in singleplayer the
// multiplayer part: LAN with its port, Command Access and Force Game Mode for the players who join. Changes wait for
// Apply Changes. On our dedicated local server the integrated server's calls become console commands and the
// LocalServer's join handling.
#include "ui/frontend.h"
#include "ui/item_ui.h"
#include "ui/modules.h"
#include "ui/options_list.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ui {

namespace {

const char* const kModes[4] = {"survival", "creative", "adventure", "spectator"};
const char* const kDifficulties[4] = {"peaceful", "easy", "normal", "hard"};

// What the dedicated server cannot be asked about, kept for this session (first: from level.dat).
struct HostWorld {
    bool known = false;
    int defaultMode = 0;
    bool allowCommands = false, guestCommands = false, forceGameMode = false;
    bool lan = false;
    u16 port = 0;
};
HostWorld g_host;

struct Options {
    int defaultMode = 0, initialDefaultMode = 0, personalMode = 0, initialPersonalMode = 0;
    bool allowCommands = false, initialAllowCommands = false;
    int difficulty = 2, initialDifficulty = 2;
    bool locked = false, initialLocked = false;
    bool lan = false, initialLan = false;
    u16 port = 0, initialPort = 0;
    bool portValid = true;
    std::string portError;  // translation key, "" none
    bool guest = false, initialGuest = false, force = false, initialForce = false;
    Field portField;
    float scroll = 0;
    bool fresh = true;  // opened (not a rebuild)
};
Options g;

bool host(UiState& s) { return s.fe->cfg.singleplayer; }  // IntegratedServer.isSingleplayerOwner
bool hardcoreWorld(UiState& s) { return s.hardcore; }
Text label(UiState& s, const char* caption, const Text& value) { return trf(s, "options.generic_value", {tr(s, caption), value}); }
Text onOffText(UiState& s, bool on) { return tr(s, on ? "options.on" : "options.off"); }
Text tooltipText(UiState& s, const char* key) { return Text(s.res.translate(key)); }

void loadHost(UiState& s) {
    if (g_host.known) return;
    g_host.known = true;
    WorldEntry w;
    if (readWorld(s.fe->cfg.worldDir, w)) g_host.defaultMode = std::clamp(w.gameMode, 0, 3), g_host.allowCommands = w.cheats;
    g_host.lan = s.fe->lanOpenPort != 0, g_host.port = s.fe->lanOpenPort;
}

bool hasSettingsChanges() {
    const bool lanPortChanged = g.lan && g.initialLan && g.port != g.initialPort;
    return g.defaultMode != g.initialDefaultMode || g.personalMode != g.initialPersonalMode || g.allowCommands != g.initialAllowCommands ||
           g.difficulty != g.initialDifficulty || g.locked != g.initialLocked || g.lan != g.initialLan || lanPortChanged ||
           g.guest != g.initialGuest || g.force != g.initialForce;
}
bool hasChanges() { return hasSettingsChanges() && (!g.lan || g.portValid); }

// updateGuestCommandAccessButton / updateForceGameModeButton: what the LAN and cheats settings allow.
void updateGuest(UiState& s) {
    if (!g.lan || !g.allowCommands) g.guest = false;
    if (hardcoreWorld(s) || !g.lan) g.force = true;
    else if (g.guest) g.force = false;
}

// tryParsePort: empty picks a free port; out of 1024..65535 or taken (but the current one): an error.
void parsePort(const std::string& v) {
    g.portError.clear();
    if (v.empty()) {
        g.port = net::suggestLanPort();
    } else {
        bool digits = v.size() <= 5;
        for (char ch : v) digits = digits && ch >= '0' && ch <= '9';
        const int n = digits ? std::atoi(v.c_str()) : 0;
        if (!digits) g.port = net::suggestLanPort(), g.portError = "lanServer.port.invalid";
        else if (n < 1024 || n > 65535) g.portError = "lanServer.port.invalid";
        else if (u16(n) != g.initialPort && !net::lanPortAvailable(u16(n))) g.portError = "lanServer.port.unavailable";
        else g.port = u16(n);
    }
    g.portValid = g.portError.empty();
}

std::string lanMotd(UiState& s) {  // like the game's LAN announcement: "<player> - <world>"
    WorldEntry w;
    const std::string level = !s.fe->cfg.worldName.empty()           ? s.fe->cfg.worldName
                              : readWorld(s.fe->cfg.worldDir, w) ? w.name
                                                                 : utf8(s.fe->cfg.worldDir.filename());
    return s.fe->cfg.playerName + " - " + level;
}

void applyChanges(UiState& s) {
    FrontEnd& f = s.fe->cfg;
    if (g.difficulty != g.initialDifficulty && s.online()) {  // ServerboundChangeDifficultyPacket
        net::Writer w;
        w.varInt(g.difficulty);
        s.client->sendPlay("change_difficulty", w);
        // The singleplayer owner may change it without being an operator (isSingleplayerOwner): the console then.
        if (host(s) && permissionLevel() < 2 && f.serverCommand) f.serverCommand(std::string("difficulty ") + kDifficulties[g.difficulty]);
    }
    if (g.locked != g.initialLocked && s.online()) {  // ServerboundLockDifficultyPacket (always true)
        net::Writer w;
        w.boolean(true);
        s.client->sendPlay("lock_difficulty", w);
        if (host(s) && f.lockDifficulty) f.lockDifficulty();
    }
    if (!host(s)) return;
    if (g.allowCommands != g.initialAllowCommands && f.setAllowCommands) f.setAllowCommands(g.allowCommands), g_host.allowCommands = g.allowCommands;
    if (g.force != g.initialForce && f.setForceGameMode) f.setForceGameMode(g.force, kModes[g.defaultMode]), g_host.forceGameMode = g.force;
    if (g.defaultMode != g.initialDefaultMode && f.serverCommand) {
        f.serverCommand(std::string("defaultgamemode ") + kModes[g.defaultMode]);
        g_host.defaultMode = g.defaultMode;
        if (g.force && f.setForceGameMode) f.setForceGameMode(true, kModes[g.defaultMode]);
    }
    if (g.personalMode != g.initialPersonalMode && f.serverCommand)
        f.serverCommand(std::string("gamemode ") + kModes[g.personalMode] + " " + f.playerName);
    if (g.guest != g.initialGuest && f.setGuestCommands) f.setGuestCommands(g.guest), g_host.guestCommands = g.guest;
    const bool lanPortChanged = g.lan && g.initialLan && g.port != g.initialPort;
    if (g.lan != g.initialLan || lanPortChanged) {  // changeMultiplayerScope
        if (g_host.lan && f.unpublishLan) {
            f.unpublishLan();
            g_host.lan = false, s.fe->lanOpenPort = 0;
            s.addChat(tr(s, "menu.multiplayerOptions.publish.stopped"));
        }
        if (g.lan) {
            std::string error;
            if (!f.publishLan || !f.publishLan(lanMotd(s), g.port, error)) {
                s.addChat(tr(s, "commands.publish.failed"));
            } else {
                g_host.lan = true, g_host.port = g.port, s.fe->lanOpenPort = g.port;
                s.addChat(translatable("menu.multiplayerOptions.publish.started.lan", {Text(std::to_string(g.port))}, s.lang()));
            }
        }
    }
}

// --- World Options ---------------------------------------------------------------------------------------------

constexpr int kHeader = 33, kFooter = 33, kGridW = 308;

// The column of section cells (padding 8, spacing 8): General with the host's rows (header, two game modes, cheats |
// difficulty, rules | restrictions) or a guest's (header, difficulty | rules, restrictions), then Multiplayer.
float contentHeight(UiState& s) {
    return host(s) ? (8 + 17 + 4 * 20 + 4 * 4 + 8) + 8 + (8 + 17 + 4 + 20 + 4 + 20 + 8) : 8 + 17 + 4 + 20 + 4 + 20 + 8;
}
float maxScroll(UiState& s) { return std::max(0.0f, contentHeight(s) - float(s.guiH() - kHeader - kFooter)); }

struct Header {
    const char* key;
    float y;
};
std::vector<Header> g_headers;

void openOptions(UiState& s);

void build(UiState& s) {
    UiState* p = &s;
    loadHost(s);
    if (g.fresh) {
        g = Options{};
        g.fresh = false;
        g.defaultMode = g.initialDefaultMode = g_host.defaultMode;
        g.personalMode = g.initialPersonalMode = std::clamp(s.gameMode(), 0, 3);
        g.allowCommands = g.initialAllowCommands = host(s) ? g_host.allowCommands : permissionLevel() >= 2;
        g.difficulty = g.initialDifficulty = std::clamp(s.difficulty, 0, 3);
        g.locked = g.initialLocked = s.difficultyLocked || s.hardcore;
        g.lan = g.initialLan = g_host.lan;
        g.port = g.initialPort = g_host.lan ? g_host.port : 0;
        g.guest = g.initialGuest = g_host.guestCommands;
        g.force = g.initialForce = g_host.forceGameMode;
        if (g.initialLan) g.portField.text.value = std::to_string(g.port);
        g.portField.text.maxLength = 5;
        updateGuest(s);
        g.initialGuest = g.guest, g.initialForce = g.force;
    }
    s.buttons.clear();
    g_headers.clear();
    const int W = s.guiW(), H = s.guiH();
    g.scroll = std::clamp(g.scroll, 0.0f, maxScroll(s));
    const float left = float(W / 2 - kGridW / 2);
    float y = float(kHeader) + 8 - g.scroll;
    auto row = [&](float h) {
        const float at = y;
        y += h + 4;
        return at;
    };
    const bool hc = hardcoreWorld(s);
    // updateButton: game rules and game modes need commands (the wanted Allow Cheats, else operator).
    const bool commandsOn = host(s) ? g.allowCommands : permissionLevel() >= 2;
    g_headers.push_back({"options.worldOptions.general.title", row(17)});
    if (host(s)) {
        auto modeButton = [&](const char* caption, const char* tip, int* value) {
            Button& b = addButton(s, left, row(20), kGridW, Text(), [p, value] {
                *value = (*value + 1) % 4;
                p->buildButtons();
            });
            b.labelFn = [p, caption, value] { return label(*p, caption, tr(*p, (std::string("selectWorld.gameMode.") + kModes[*value]).c_str())); };
            b.active = !hc && commandsOn;
            const char* t = hc ? "options.worldOptions.game_mode.disabled.tooltip"
                               : commandsOn ? tip : "options.worldOptions.game_mode.disabled.operator.tooltip";
            b.tooltip = [p, t] { return tooltipText(*p, t); };
        };
        modeButton("options.worldOptions.game_mode", "options.worldOptions.game_mode.tooltip", &g.defaultMode);
        modeButton("options.worldOptions.personal_game_mode", "options.worldOptions.personal_game_mode.tooltip", &g.personalMode);
        const float ry = row(20);
        Button& cheats = addButton(s, left, ry, 150, Text(), [p] {  // Allow Cheats
            g.allowCommands = !g.allowCommands;
            if (!g.allowCommands) g.defaultMode = g.initialDefaultMode, g.personalMode = g.initialPersonalMode;
            updateGuest(*p);
            p->buildButtons();
        });
        cheats.labelFn = [p] { return label(*p, "selectWorld.allowCommands", onOffText(*p, g.allowCommands)); };
        const bool cheatsLocked = hc && permissionLevel() < 4;
        cheats.active = !cheatsLocked;
        const char* ct = cheatsLocked ? "options.worldOptions.allow_commands.disabled.tooltip" : "options.worldOptions.allow_commands.tooltip";
        cheats.tooltip = [p, ct] { return tooltipText(*p, ct); };
        y = ry;  // the difficulty shares the row
    }
    {  // DifficultyButtons: the cycle button (130) and the lock (20)
        const float dx = host(s) ? left + 158 : left, dy = row(20);
        const bool may = permissionLevel() >= 2 || host(s);
        const bool lockedNow = s.difficultyLocked || s.hardcore;
        Button& d = addButton(s, dx, dy, 130, Text(), [p] {
            g.difficulty = (g.difficulty + 1) % 4;
            p->buildButtons();
        });
        d.labelFn = [p] { return label(*p, "options.difficulty", tr(*p, (std::string("options.difficulty.") + kDifficulties[g.difficulty]).c_str())); };
        d.active = may && !lockedNow;
        const char* dt = !may ? "options.worldOptions.difficulty.disabled.operator.tooltip"
                         : s.difficultyLocked ? "options.worldOptions.difficulty.disabled.locked.tooltip"
                         : s.hardcore ? "options.worldOptions.difficulty.disabled.hardcore.tooltip"
                                      : nullptr;
        if (dt) d.tooltip = [p, dt] { return tooltipText(*p, dt); };
        Button& lock = addButton(s, dx + 130, dy, 20, Text(), [p] {
            g.locked = !g.locked;
            p->buildButtons();
        });
        lock.active = may && !lockedNow;
        lock.icon = "lock";  // drawn in draw() (LockIconButton sprites)
        if (!host(s)) {
            Button& rules = addButton(s, left + 158, dy, 150, tr(s, "editGamerule.inGame.button"), [p] { p->openScreen(Screen::InWorldGameRules); });
            rules.active = !hc && commandsOn;
            const char* rt = hc ? "editGamerule.inGame.disabled.hardcore.tooltip" : commandsOn ? nullptr : "editGamerule.inGame.disabled.tooltip";
            if (rt) rules.tooltip = [p, rt] { return tooltipText(*p, rt); };
            addButton(s, left, row(20), 150, tr(s, "restrictions_screen.button"), [p] { p->restrictionsParent = Screen::WorldOptions, p->openScreen(Screen::Restrictions); });
        }
    }
    if (host(s)) {
        const float ry = row(20);
        Button& rules = addButton(s, left, ry, 150, tr(s, "editGamerule.inGame.button"), [p] { p->openScreen(Screen::InWorldGameRules); });
        rules.active = !hc && commandsOn;
        const char* rt = hc ? "editGamerule.inGame.disabled.hardcore.tooltip" : commandsOn ? nullptr : "editGamerule.inGame.disabled.tooltip";
        if (rt) rules.tooltip = [p, rt] { return tooltipText(*p, rt); };
        addButton(s, left + 158, ry, 150, tr(s, "restrictions_screen.button"), [p] { p->restrictionsParent = Screen::WorldOptions, p->openScreen(Screen::Restrictions); });
        // The multiplayer part
        y += 8 - 4 + 16;  // the next section: spacing 8 and the cells' padding
        g_headers.push_back({"options.worldOptions.multiplayer.title", row(17)});
        const float ly = row(20);
        Button& lan = addButton(s, left, ly, 150, Text(), [p] {
            g.lan = !g.lan;
            if (g.lan && !g.initialLan && g.portField.text.value.empty()) parsePort("");
            updateGuest(*p);
            p->buildButtons();
        });
        lan.labelFn = [p] { return label(*p, "menu.multiplayerOptions.lan", onOffText(*p, g.lan)); };
        lan.tooltip = [p] { return tooltipText(*p, g.lan ? "menu.multiplayerOptions.network.lan.tooltip" : "menu.multiplayerOptions.network.off.tooltip"); };
        g.portField.x = left + 158 + 1, g.portField.y = ly + 1, g.portField.w = 148, g.portField.h = 18;
        g.portField.hint = g.lan ? std::to_string(g.port) : s.res.translate("lanServer.port");
        setFields(s, g.lan ? std::vector<Field*>{&g.portField} : std::vector<Field*>{}, -1);
        const float gy = row(20);
        Button& guest = addButton(s, left, gy, 150, Text(), [p] {
            g.guest = !g.guest;
            updateGuest(*p);
            p->buildButtons();
        });
        guest.labelFn = [p] { return label(*p, "options.worldOptions.guest.command_access", onOffText(*p, g.guest)); };
        guest.active = g.lan && g.allowCommands;
        const char* gt = !g.lan ? "options.worldOptions.guest.command_access.disabled.scope.tooltip"
                         : !g.allowCommands ? "options.worldOptions.guest.command_access.disabled.commands.tooltip"
                                            : "options.worldOptions.guest.command_access.tooltip";
        guest.tooltip = [p, gt] { return tooltipText(*p, gt); };
        Button& force = addButton(s, left + 158, gy, 150, Text(), [p] {
            g.force = !g.force;
            p->buildButtons();
        });
        force.labelFn = [p] { return label(*p, "options.worldOptions.guest.force_game_mode", onOffText(*p, g.force)); };
        force.active = g.lan && !g.guest && !hc;
        const char* ft = hc ? "options.worldOptions.guest.force_game_mode.off.hardcore.tooltip"
                         : !g.lan ? "options.worldOptions.guest.force_game_mode.off.scope.tooltip"
                         : g.guest ? "options.worldOptions.guest.force_game_mode.off.commands.tooltip"
                         : g.force ? "options.worldOptions.guest.force_game_mode.on.tooltip"
                                   : "options.worldOptions.guest.force_game_mode.off.tooltip";
        force.tooltip = [p, ft] { return tooltipText(*p, ft); };
    } else {
        setFields(s, {}, -1);
    }
    // Footer: Apply Changes (with the lock confirmation) and Cancel.
    Button& apply = addButton(s, float(W / 2 - 154), float(H - kFooter + 6), 150, tr(s, "menu.multiplayerOptions.applyChanges"), [p] {
        if (g.locked != g.initialLocked) {
            const Text name = tr(*p, (std::string("options.difficulty.") + kDifficulties[g.difficulty]).c_str());
            showConfirm(*p, tr(*p, "difficulty.lock.title"), trf(*p, "difficulty.lock.question", {name}), p->res.translate("gui.yes"),
                        [p] {
                            applyChanges(*p);
                            g.fresh = true;
                            p->openScreen(Screen::Pause);
                        },
                        Screen::WorldOptions);
            return;
        }
        applyChanges(*p);
        g.fresh = true;
        p->openScreen(Screen::Pause);
    });
    apply.active = hasChanges();
    addButton(s, float(W / 2 + 4), float(H - kFooter + 6), 150, tr(s, "gui.cancel"), [p] {
        g.fresh = true;
        p->openScreen(Screen::Pause);
    });
}

void draw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "options.worldOptions.title"), float(W / 2), float((kHeader - 9) / 2 + 1), 0xFFFFFFFF);
    c.pushScissor(0, float(kHeader), float(W), float(H - kFooter));
    for (const Header& h : g_headers) {  // createCategoryHeader: bold underlined, centred
        Style st;
        st.bold = 1, st.underlined = 1;
        s.drawCentered(c, Text(s.res.translate(h.key), st), float(W / 2), h.y + 4, 0xFFFFFFFF);
    }
    for (size_t i = 0; i + 2 < s.buttons.size(); ++i) {
        Button& b = s.buttons[i];
        if (b.icon == "lock") {  // LockIconButton
            const bool hover = b.active && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h;
            const bool locked = g.locked;
            const char* sp = !b.active ? (locked ? "widget/locked_button_disabled" : "widget/unlocked_button_disabled")
                             : hover   ? (locked ? "widget/locked_button_highlighted" : "widget/unlocked_button_highlighted")
                                       : (locked ? "widget/locked_button" : "widget/unlocked_button");
            blitSprite(c, s.sprites.sprite(sp), b.x, b.y, b.w, b.h);
        } else {
            s.drawButton(c, i, s.mouseY >= float(kHeader) && s.mouseY < float(H - kFooter));
        }
    }
    if (host(s) && g.lan) {
        drawFields(s, c);
    } else if (host(s)) {  // the inactive port box
        const Field& f = g.portField;
        blitSprite(c, s.sprites.sprite("widget/text_field"), f.x - 1, f.y - 1, f.w + 2, f.h + 2);
        s.drawText(c, Text(s.res.translate("lanServer.port")), f.x + 3, f.y + (f.h - 8) / 2, 0xFF707070, true);
    }
    c.popScissor();
    if (maxScroll(s) > 0) {  // the scroll area's bar
        const float top = float(kHeader), bottom = float(H - kFooter), h = bottom - top;
        const float bar = std::clamp(h * h / contentHeight(s), 32.0f, h - 8);
        const float sx = float(W / 2 + kGridW / 2 + 8 + 2);
        blitSprite(c, s.sprites.sprite("widget/scroller_background"), sx, top, 6, h);
        blitSprite(c, s.sprites.sprite("widget/scroller"), sx, top + (h - bar) * g.scroll / maxScroll(s), 6, bar);
        s.scrollbarCursor(sx, top, h);
    }
    s.drawButton(c, s.buttons.size() - 2);
    s.drawButton(c, s.buttons.size() - 1);
    std::vector<Text> tip;
    for (const Button& b : s.buttons)
        if (b.tooltip && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h &&
            (b.y >= float(H - kFooter) || (s.mouseY >= float(kHeader) && s.mouseY < float(H - kFooter))))
            tip = s.font.split(b.tooltip(), 170);
    if (host(s) && g.lan && !g.portValid) {
        const Field& f = g.portField;
        if (s.mouseX >= f.x && s.mouseY >= f.y && s.mouseX < f.x + f.w && s.mouseY < f.y + f.h)
            tip = s.font.split(Text(s.res.translate(g.portError)), 170);
    }
    if (!tip.empty()) s.drawTooltip(c, tip, s.mouseX, s.mouseY);
}

void input(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {
        g.fresh = true;
        return s.openScreen(Screen::Pause);
    }
    if (e.type == E::Wheel) {
        g.scroll = std::clamp(g.scroll - float(e.code) * 10.0f, 0.0f, maxScroll(s));
        return s.buildButtons();
    }
    if (host(s) && g.lan) {
        const std::string before = g.portField.text.value;
        if (fieldsInput(s, e)) {
            if (g.portField.text.value != before) {
                parsePort(g.portField.text.value);
                if (!s.buttons.empty()) s.buttons[s.buttons.size() - 2].active = hasChanges();
            }
            return;
        }
    }
    if (e.type == E::Button && e.down && (s.mouseY < float(kHeader) || s.mouseY >= float(s.guiH() - kFooter))) {
        s.buttonsInput(e, s.buttons.size() - 2);  // only the footer outside the scroll area
        return;
    }
    s.buttonsInput(e);
}

void closed(UiState& s) { setFields(s, {}, -1); }

const ScreenRegistrar kWorldOptions({Screen::WorldOptions, build, draw, input, closed});

// --- In-world game rules -----------------------------------------------------------------------------------------

struct Rules {
    bool requested = false, received = false, haveDefs = false;
    std::map<std::string, std::string> server;  // the values the server sent
    std::map<std::string, std::string> initial, edit;
    struct Row {
        std::string category;
        std::string id;  // empty: a category header
    };
    std::vector<Row> rows;
    std::vector<Field> fields;
    std::vector<std::string> fieldIds;
    Field search;
    float scroll = 0;
};
Rules r;

constexpr int kRulesHeader = 36, kRowH = 24;  // HeaderAndFooterLayout(12 + 9 + 15, 33)

const GameRuleInfo* info(UiState& s, const std::string& id) {
    if (const auto* all = gameRuleInfos(s))
        for (const GameRuleInfo& i : *all)
            if (i.id == id) return &i;
    return nullptr;
}
bool isBoolean(UiState& s, const std::string& id) {
    if (const GameRuleInfo* i = info(s, id)) return i->boolean;
    auto it = r.server.find(id);
    return it != r.server.end() && (it->second == "true" || it->second == "false");
}
bool valid(UiState& s, const std::string& id, const std::string& v) {
    if (isBoolean(s, id)) return v == "true" || v == "false";
    if (v.empty() || v.size() > 11) return false;
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    const GameRuleInfo* i = info(s, id);
    return *end == 0 && (!i || (n >= i->min && n <= i->max));
}
std::string value(const std::string& id) {
    auto it = r.edit.find(id);
    return it != r.edit.end() ? it->second : r.initial[id];
}
bool pending(UiState& s) {
    for (const auto& [id, v] : r.edit)
        if (v != r.initial[id] && valid(s, id, v)) return true;
    return false;
}
bool allValid(UiState& s) {
    for (const auto& [id, v] : r.edit)
        if (!valid(s, id, v)) return false;
    return true;
}

void filter(UiState& s) {
    std::string q = r.search.text.value;
    auto lower = [](std::string v) {
        for (char& ch : v) ch = char(std::tolower(u8(ch)));
        return v;
    };
    q = lower(q);
    std::map<std::string, std::vector<std::string>> byCategory;
    for (const auto& [id, v] : r.server) {
        const GameRuleInfo* i = info(s, id);
        const std::string cat = i ? i->category : "minecraft:misc";
        const std::string catName = gameRuleCategoryName(s, cat);
        if (!q.empty() && lower(id).find(q) == std::string::npos && lower(gameRuleName(s, id)).find(q) == std::string::npos &&
            lower(catName).find(q) == std::string::npos && lower(gameRuleDescription(s, id)).find(q) == std::string::npos)
            continue;
        byCategory[cat].push_back(id);
    }
    r.rows.clear();
    for (auto& [cat, ids] : byCategory) {
        r.rows.push_back({cat, ""});
        std::sort(ids.begin(), ids.end());
        for (const std::string& id : ids) r.rows.push_back({cat, id});
    }
}

float listTop() { return float(kRulesHeader); }
float listBottom(UiState& s) { return float(s.guiH() - 33); }
float rowY(size_t i) { return listTop() + 4 + float(i * kRowH) - r.scroll; }
float maxRuleScroll(UiState& s) { return std::max(0.0f, float(r.rows.size() * kRowH + 8) - (listBottom(s) - listTop())); }

void syncFields() {
    for (size_t i = 0; i < r.fields.size() && i < r.fieldIds.size(); ++i) r.edit[r.fieldIds[i]] = r.fields[i].text.value;
}

void leaveRules(UiState& s) {
    r = Rules{};
    s.openScreen(Screen::WorldOptions);
}

void rulesBuild(UiState& s) {
    UiState* p = &s;
    if (!r.requested && s.online()) {  // ServerboundClientCommandPacket REQUEST_GAMERULE_VALUES
        r.requested = true;
        net::Writer w;
        w.varInt(2);
        s.client->sendPlay("client_command", w);
    }
    syncFields();
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    r.search.x = float(W / 2 - 100), r.search.y = 17, r.search.w = 200, r.search.h = 15;  // the header: title, then search
    r.search.hint = s.res.translate("gui.game_rule.search");
    std::vector<Field*> fields{&r.search};
    r.fields.clear(), r.fieldIds.clear();
    if (r.received) {
        filter(s);
        r.scroll = std::clamp(r.scroll, 0.0f, maxRuleScroll(s));
        r.fields.reserve(r.rows.size());
        const float left = float(W / 2 - 155), width = 310;
        for (size_t i = 0; i < r.rows.size(); ++i) {
            const Rules::Row& row = r.rows[i];
            const float y = rowY(i);
            if (row.id.empty() || y < listTop() || y + kRowH > listBottom(s) + 1) continue;
            const float x = left + width - 4 - 45;
            if (isBoolean(s, row.id)) {
                Button& b = addButton(s, x, y, 44, Text(), [id = row.id] { r.edit[id] = value(id) == "true" ? "false" : "true"; });
                b.labelFn = [p, id = row.id] { return onOffText(*p, value(id) == "true"); };
            } else {
                Field fl;
                fl.x = x, fl.y = y, fl.w = 44, fl.h = 20;
                fl.text.value = value(row.id);
                fl.text.maxLength = 11;
                fl.text.cursor = fl.text.selection = int(fl.text.value.size());
                r.fields.push_back(std::move(fl));
                r.fieldIds.push_back(row.id);
            }
        }
    }
    for (Field& fl : r.fields) fields.push_back(&fl);
    setFields(s, fields, 0);
    addButton(s, float(W / 2 - 154), float(H - 27), 150, tr(s, "gui.done"), [p] {  // onDone: the changed rules
        syncFields();
        if (!allValid(*p)) return;
        std::vector<std::pair<std::string, std::string>> changed;
        for (const auto& [id, v] : r.edit)
            if (v != r.initial[id]) changed.push_back({id, v});
        if (!changed.empty() && p->online()) {  // ServerboundSetGameRulePacket
            net::Writer w;
            w.varInt(int(changed.size()));
            for (const auto& [id, v] : changed) w.str(id), w.str(v);
            p->client->sendPlay("set_game_rule", w);
        }
        leaveRules(*p);
    }, r.received && allValid(s));
    addButton(s, float(W / 2 + 4), float(H - 27), 150, tr(s, "gui.cancel"), [p] {
        syncFields();
        if (pending(*p)) {  // the discard confirmation
            showConfirm(*p, tr(*p, "editGamerule.inGame.discardChanges.title"), tr(*p, "editGamerule.inGame.discardChanges.message"),
                        p->res.translate("gui.yes"), [p] { leaveRules(*p); }, Screen::InWorldGameRules);
            return;
        }
        leaveRules(*p);
    });
}

void rulesDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    syncFields();
    if (r.received && !r.haveDefs && gameRuleInfos(s)) {  // the categories and types arrived: sort the rules again
        r.haveDefs = true;
        s.buildButtons();
    }
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "editGamerule.title"), float(W / 2), 4, 0xFFFFFFFF);
    const float top = listTop(), bottom = listBottom(s);
    listBackground(s, c, top, bottom);
    std::vector<Text> tooltip;
    if (!r.received) {  // LoadingDotsWidget
        static const char* const kDots[4] = {"O o o", "o O o", "o o O", "o O o"};
        s.drawCentered(c, tr(s, "editGamerule.inGame.downloadingGamerules"), float(W / 2), (top + bottom) / 2 - 9, 0xFFFFFFFF);
        s.drawCentered(c, Text(kDots[int(s.clock * 1000.0 / 300.0) % 4]), float(W / 2), (top + bottom) / 2 + 3, 0xFF808080);
    } else {
        c.pushScissor(0, top, float(W), bottom);
        const float left = float(W / 2 - 155), width = 310;
        for (size_t i = 0; i < r.rows.size(); ++i) {
            const float y = rowY(i);
            if (y + kRowH < top || y > bottom) continue;
            const Rules::Row& row = r.rows[i];
            if (row.id.empty()) {  // CategoryRuleEntry: bold yellow
                Style st;
                st.bold = 1;
                s.drawCentered(c, Text(gameRuleCategoryName(s, row.category), st), float(W / 2), y + 6, 0xFFFFFF55);
                continue;
            }
            const std::vector<Text> name = s.font.split(Text(gameRuleName(s, row.id)), 170);
            const float x = left + 4;
            if (name.size() == 1) s.drawText(c, name[0], x, y + 5, 0xFFFFFFFF, true);
            else
                for (size_t k = 0; k < name.size() && k < 2; ++k) s.drawText(c, name[k], x, y + float(k) * 10, 0xFFFFFFFF, true);
            if (!isBoolean(s, row.id) && !valid(s, row.id, value(row.id)))  // an invalid number: red frame
                c.fill(left + width - 50, y - 1, left + width - 3, y + 21, 0xFFFF0000);
            if (s.mouseX >= x && s.mouseX < x + 170 && s.mouseY >= y && s.mouseY < y + 20 && s.mouseY >= top && s.mouseY < bottom) {
                tooltip.push_back(Text(row.id, colorStyle(0xFFFF55)));
                for (const Text& t : s.font.split(Text(gameRuleDescription(s, row.id)), 150))
                    if (!t.empty()) tooltip.push_back(t);
                if (const GameRuleInfo* in = info(s, row.id))
                    tooltip.push_back(trf(s, "editGamerule.default", {Text(in->def)}).withStyle(colorStyle(0xAAAAAA)));
            }
        }
        c.popScissor();
        if (maxRuleScroll(s) > 0) {
            const float h = bottom - top, bar = std::clamp(h * h / float(r.rows.size() * kRowH + 8), 32.0f, h - 8);
            const float sx = left + width + 6 + 2;
            blitSprite(c, s.sprites.sprite("widget/scroller_background"), sx, top, 6, h);
            blitSprite(c, s.sprites.sprite("widget/scroller"), sx, top + (h - bar) * r.scroll / maxRuleScroll(s), 6, bar);
            s.scrollbarCursor(sx, top, h);
        }
    }
    drawFields(s, c);
    s.drawButtons(c);
    if (!tooltip.empty()) s.drawTooltip(c, tooltip, s.mouseX, s.mouseY);
}

void rulesInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {
        if (s.buttons.size() >= 2 && s.buttons.back().onPress) s.buttons.back().onPress();  // Cancel
        return;
    }
    if (e.type == E::Wheel && s.mouseY >= listTop() && s.mouseY < listBottom(s)) {
        syncFields();
        r.scroll = std::clamp(r.scroll - float(e.code) * float(kRowH / 2), 0.0f, maxRuleScroll(s));
        return s.buildButtons();
    }
    const std::string before = r.search.text.value;
    if (fieldsInput(s, e)) {
        if (r.search.text.value != before) {
            r.scroll = 0;
            s.buildButtons();
        } else {
            syncFields();
            if (s.buttons.size() >= 2) s.buttons[s.buttons.size() - 2].active = r.received && allValid(s);
        }
        return;
    }
    s.buttonsInput(e);
}

const ScreenRegistrar kRules({Screen::InWorldGameRules, rulesBuild, rulesDraw, rulesInput, closed});

// --- Restrictions --------------------------------------------------------------------------------------------------

// Minecraft.computeChatAbilities: the Chat option's restrictions (offline: no launcher / profile restrictions).
struct Restriction {
    const char* display;
    const char* action;  // "" none
};
std::vector<Restriction> restrictions(UiState& s) {
    if (s.settings.chatVisibility == 2) return {{"chat_restriction.chat_and_commands_disabled_by_options", "chat_restriction.chat_disabled_by_options.action"}};
    if (s.settings.chatVisibility == 1) return {{"chat_restriction.chat_disabled_by_options", "chat_restriction.chat_disabled_by_options.action"}};
    return {};
}

void restrictionsBack(UiState& s) {  // onClose: the screen it was opened from
    if (s.restrictionsParent == Screen::Chat) s.openChat(false);
    else s.openScreen(s.restrictionsParent);
}

void restrictionsBuild(UiState& s) {
    UiState* p = &s;
    s.buttons.clear();
    const int W = s.guiW(), H = s.guiH();
    float y = 33;
    for (const Restriction& rs : restrictions(s)) {
        y += float(s.font.split(Text(s.res.translate(rs.display)), 250).size() * 9 + 8);
        if (*rs.action) {
            addButton(s, float(W / 2 - 100), y, 200, tr(s, rs.action), [p] { p->openScreen(Screen::ChatOptions); });
            y += 20;
        }
    }
    addButton(s, float(W / 2 - 100), float(H - 27), 200, tr(s, "gui.done"), [p] { restrictionsBack(*p); });
}

void restrictionsDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth();
    menuBackground(s, c);
    s.drawCentered(c, tr(s, "restrictions_screen.title"), float(W / 2), 12, 0xFFFFFFFF);
    float y = 33;
    const std::vector<Restriction> list = restrictions(s);
    for (const Restriction& rs : list) {
        Style st = colorStyle(0xFF5555);
        st.bold = 1;
        for (const Text& line : s.font.split(Text(s.res.translate(rs.display), st), 250)) {
            s.drawCentered(c, line, float(W / 2), y + 4, 0xFFFFFFFF);
            y += 9;
        }
        y += 8;
        if (*rs.action) y += 20;
    }
    if (!list.empty()) y += 10;
    const bool chatAll = s.settings.chatVisibility == 0, commands = s.settings.chatVisibility != 2;
    const std::pair<const char*, bool> perms[4] = {{"send_messages", chatAll}, {"send_commands", commands},
                                                   {"receive_system_messages", commands}, {"receive_player_messages", chatAll}};
    y += 4;
    for (const auto& [id, allowed] : perms) {
        const std::string key = std::string("restrictions_screen.permission.") + id + (allowed ? ".allowed" : ".denied");
        s.drawCentered(c, Text(s.res.translate(key), colorStyle(allowed ? 0x55FF55 : 0xFF5555)), float(W / 2), y, 0xFFFFFFFF);
        y += 9;
    }
    s.drawButtons(c);
}

void restrictionsInput(UiState& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return restrictionsBack(s);
    s.buttonsInput(e);
}

const ScreenRegistrar kRestrictions({Screen::Restrictions, restrictionsBuild, restrictionsDraw, restrictionsInput, nullptr});

// ClientboundGameRuleValuesPacket: the rules screen's values (only while it waits for them).
void registerHandlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    client.onPlay("game_rule_values", [sp](net::Reader& rd) {
        auto values = std::make_shared<std::map<std::string, std::string>>();
        const int n = rd.varInt();
        for (int i = 0; i < n && rd.ok; ++i) {
            std::string key(rd.str());
            std::string v(rd.str());
            (*values)[key] = v;
        }
        if (!rd.ok) return;
        sp->post([sp, values] {
            if (sp->screen != Screen::InWorldGameRules || r.received) return;
            r.received = true;
            r.server = *values;
            r.initial = *values;
            sp->buildButtons();
        });
    });
}

void attached(UiState&) { g_host = HostWorld{}, g.fresh = true, r = Rules{}; }

const UiModuleRegistrar kModule({.name = "world_options", .registerHandlers = registerHandlers, .attached = attached});

}  // namespace

}  // namespace ui
