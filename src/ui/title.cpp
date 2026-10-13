// Front end: the title screen over a live world panorama, loading / message / confirmation screens, Open to LAN, and
// switching instances (each world and the title screen are their own process, platform/launch.h).
#include "ui/frontend.h"
#include "ui/toasts.h"

#include "audio/audio.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <random>

namespace ui {

namespace {

constexpr float kPi = 3.14159265f;

std::string pickSplash(const Resources& res, const std::string& playerName) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    // SplashManager: the dates first.
    if (tm.tm_mon == 11 && tm.tm_mday == 24) return "Merry X-mas!";
    if (tm.tm_mon == 0 && tm.tm_mday == 1) return "Happy new year!";
    if (tm.tm_mon == 9 && tm.tm_mday == 31) return "OOoooOOOoooo! Spooky!";
    std::vector<u8> bytes;
    if (!res.read("assets/minecraft/texts/splashes.txt", bytes)) return {};
    // SplashManager.prepare: every line trimmed, without the one that "will never appear"
    std::vector<std::string> lines;
    std::string cur;
    auto add = [&] {
        const size_t a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
        std::string line = a == std::string::npos ? std::string() : cur.substr(a, b - a + 1);
        if (line != "This message will never appear on the splash screen, isn't that weird?") lines.push_back(std::move(line));
        cur.clear();
    };
    for (u8 b : bytes) {
        if (b == '\n') add();
        else cur += char(b);
    }
    if (!cur.empty()) add();
    if (lines.empty()) return {};
    std::mt19937 rng(std::random_device{}());
    auto pick = [&] { return std::uniform_int_distribution<size_t>(0, lines.size() - 1)(rng); };
    if (!playerName.empty() && pick() == 42) {  // "<NAME> IS YOU"
        std::string upper = playerName;
        for (char& ch : upper) ch = char(std::toupper(u8(ch)));
        return upper + " IS YOU";
    }
    return lines[pick()];
}

void tileTexture(Canvas& c, const Sprite* t, float x0, float y0, float x1, float y1, float tile) {
    if (!t) return;
    for (float y = y0; y < y1; y += tile)
        for (float x = x0; x < x1; x += tile) {
            const float w = std::min(tile, x1 - x), h = std::min(tile, y1 - y);
            blitTexture(c, t, x, y, 0, 0, w, h, int(tile), int(tile));
        }
}

// The next instance's log lines -> what the loading screen says.
void loadingLine(State& s, std::string line) {
    FrontEndState& f = *s.fe;
    if (line.starts_with("[")) line.erase(0, std::min(line.size(), line.find("] ") + 2));  // MCPP_LOG_TIME stamps
    auto has = [&](const char* t) { return line.find(t) != std::string::npos; };
    auto at = [&](float p, std::string status) {
        f.progress = std::max(f.progress, p);
        if (!status.empty()) f.loadingStatus = std::move(status);
    };
    if (line.starts_with("downloading the official server")) at(0.05f, "Downloading the game's server (one time)...");
    else if (line.starts_with("server  starting the local server")) at(0.25f, "Starting the local server...");
    else if (line.starts_with("server  upgrading")) at(0.25f, "Upgrading the world to this version...");
    else if (has("Preparing spawn area")) {
        const size_t colon = line.rfind(':');
        const int pct = colon == std::string::npos ? 0 : std::atoi(line.c_str() + colon + 1);
        at(0.3f + 0.4f * float(std::clamp(pct, 0, 100)) / 100.0f,
           formatJava(s.res.translate("menu.preparingSpawn"), {std::to_string(std::clamp(pct, 0, 100))}));
    } else if (line.starts_with("server: ") && has("Preparing level")) at(0.3f, s.res.translate("createWorld.preparing"));
    else if (line.starts_with("server: ") && has("Done (")) at(0.7f, s.res.translate("multiplayer.downloadingTerrain"));
    else if (line.starts_with("assets ")) at(0.12f, "Loading resources...");
    else if (line.starts_with("friends: connecting to")) at(0.04f, "Connecting to your friend...");
    else if (line.starts_with("friends: connected to")) at(0.1f, "Connected to your friend");
    else if (line.starts_with("ui ")) at(0.2f, "");
    else if (line.starts_with("server ") && has("joined")) at(0.6f, s.res.translate("connect.joining"));
    else if (line.starts_with("net: logged in as")) {  // ClientHandshakePacketListenerImpl: JOINING
        at(0.5f, s.res.translate("connect.joining"));
        f.connectStatus = f.loadingStatus;
    }
    else if (line.starts_with("net: joined")) at(0.75f, s.res.translate("multiplayer.downloadingTerrain"));
    else if (line.starts_with("world ")) at(0.75f, s.res.translate("multiplayer.downloadingTerrain"));
    else if (line.starts_with("lod ") || line.starts_with("detail ")) at(0.85f, "Building terrain...");
    else if (line.starts_with("total ")) at(0.95f, "");
}

// Joining a server (or a transfer): ConnectScreen until the terrain loads (its status line, Cancel, Esc ignored).
bool connectScreen(const FrontEndState& f) { return f.loadingCancel && f.failBack == Screen::Servers && f.progress < 0.75f; }

void buildLoading(State& s) {
    FrontEndState& f = *s.fe;
    State* p = &s;
    f.onBack = nullptr;
    if (f.optimizeScreen) {  // OptimizeWorldScreen: Cancel at H / 4 + 150, Esc ignored
        if (f.loadingCancel)
            addButton(s, float(s.guiW() / 2 - 100), float(s.guiH() / 4 + 150), 200, tr(s, "gui.cancel"), [p] {
                if (auto fn = p->fe->loadingCancelFn) fn();
            });
        return;
    }
    if (connectScreen(f)) {
        addButton(s, float(s.guiW() / 2 - 100), float(s.guiH() / 4 + 120 + 12), 200, tr(s, "gui.cancel"), [p] {
            FrontEndState& fe = *p->fe;
            if (fe.child) fe.child->cancel(), fe.child.reset();
            // setScreen(parent): the server list (from a world, a transfer's: the title instance's)
            if (fe.cfg.menu) p->openScreen(Screen::Servers);
            else requestTitle(*p, "servers");
        });
        return;  // shouldCloseOnEsc: false
    }
    if (f.loadingCancel) {
        addButton(s, float(s.guiW() / 2 - 100), float(s.guiH() / 2 + 40), 200, tr(s, "gui.cancel"), [p] {
            FrontEndState& fe = *p->fe;
            if (auto fn = fe.loadingCancelFn) fn();
            else if (fe.child) fe.child->cancel();
        });
        f.onBack = [p] {
            if (auto fn = p->fe->loadingCancelFn) fn();
            else if (p->fe->child) p->fe->child->cancel();
        };
    }
}

void drawLoading(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth(), H = c.guiHeight();
    menuBackground(s, c);
    if (f.optimizeScreen) {  // OptimizeWorldScreen.extractRenderState
        s.drawCentered(c, f.loadingTitle, float(W / 2), 20, 0xFFFFFFFF);
        const int x0 = W / 2 - 150, x1 = W / 2 + 150, y0 = H / 4 + 100, y1 = y0 + 10;
        s.drawCentered(c, Text(f.loadingStatus), float(W / 2), float(y0 - 9 - 2), 0xFFA0A0A0);
        if (f.upgradeTotal > 0) {
            // The server's log counts the chunks done (converted or skipped) over all dimensions: one bar, the total
            // line and the counter (no per-dimension split or converted / skipped lines).
            c.fill(float(x0 - 1), float(y0 - 1), float(x1 + 1), float(y1 + 1), 0xFF000000);
            s.drawText(c, trf(s, "optimizeWorld.info.total", {Text(std::to_string(f.upgradeTotal))}), float(x0), 40 + (9 + 3) * 2,
                       0xFFA0A0A0, true);
            const float pct = std::clamp(float(f.upgradeDone) / float(f.upgradeTotal), 0.0f, 1.0f);
            c.fill(float(x0), float(y0), float(x0 + int(std::floor(pct * float(x1 - x0)))), float(y1), 0xFF336622);
            s.drawCentered(c, trf(s, "optimizeWorld.progress.counter", {Text(std::to_string(f.upgradeDone)), Text(std::to_string(f.upgradeTotal))}),
                           float(W / 2), float(y0 + 2 * 9 + 2), 0xFFA0A0A0);
            s.drawCentered(c, trf(s, "optimizeWorld.progress.percentage", {Text(std::to_string(int(std::floor(pct * 100.0f))))}),
                           float(W / 2), float(y0 + (y1 - y0) / 2 - 9 / 2), 0xFFA0A0A0);
        }
        s.drawButtons(c);
        return;
    }
    if (connectScreen(f)) {  // only the connection's own stages (not the instance's start-up)
        s.drawCentered(c, f.connectStatus.empty() ? f.loadingTitle : Text(f.connectStatus), float(W / 2), float(H / 2 - 50), 0xFFFFFFFF);
        s.drawButtons(c);
        return;
    }
    s.drawCentered(c, f.loadingTitle, float(W / 2), float(H / 2 - 30), 0xFFFFFFFF);
    std::string status = f.loadingStatus;
    if (f.progress < 0) {  // no steps to show: dots (LoadingDotsText)
        static const char* dots[] = {"O o o", "o O o", "o o O", "o O o"};
        status = dots[int(s.clock * 3.0) % 4];
    }
    s.drawCentered(c, Text(status), float(W / 2), float(H / 2 - 14), 0xFFA0A0A0);
    if (f.progress >= 0) {
        // Moves on slowly between the steps the next instance reports, so it never looks stuck.
        const double t = s.clock - f.loadingStart;
        const float shown = std::min(0.97f, std::max(f.progress, f.progress + float(1.0 - std::exp(-t / 20.0)) * 0.08f));
        const float x0 = float(W / 2 - 100), x1 = float(W / 2 + 100), y0 = float(H / 2 + 2), y1 = y0 + 6;
        c.fill(x0, y0, x1, y0 + 1, 0xFFFFFFFF), c.fill(x0, y1 - 1, x1, y1, 0xFFFFFFFF);
        c.fill(x0, y0, x0 + 1, y1, 0xFFFFFFFF), c.fill(x1 - 1, y0, x1, y1, 0xFFFFFFFF);
        c.fill(x0 + 2, y0 + 2, x0 + 2 + (x1 - x0 - 4) * shown, y1 - 2, 0xFFFFFFFF);
    }
    s.drawButtons(c);
}

// DisconnectedScreen: a centred column (title and reason padded 10, the button 2); Esc does nothing. As AlertScreen
// (messageAlert): the title at 70, the text from 90, a 150 wide button at clamp(90 + text + 12, H / 6 + 96, H - 24).
int messageTop(State& s, int lines) { return (s.guiH() - (9 + 20 + lines * 9 + 20 + 24)) / 2; }

void buildMessage(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW(), H = s.guiH();
    const int lines = int(s.font.split(f.messageText, W - 50).size());
    State* p = &s;
    auto act = [p] {
        auto fn = p->fe->messageAction;
        if (fn) fn();
    };
    if (f.messageAlert) {
        addButton(s, float((W - 150) / 2), float(std::clamp(90 + lines * 9 + 12, H / 6 + 96, H - 24)), 150, Text(f.messageButton), act);
        f.onBack = act;  // AlertScreen.shouldCloseOnEsc
        return;
    }
    addButton(s, float(W / 2 - 100), float(messageTop(s, lines) + 29 + lines * 9 + 20 + 2), 200, Text(f.messageButton), act);
}

void drawMessage(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth();
    menuBackground(s, c);
    const std::vector<Text> lines = s.font.split(f.messageText, W - 50);
    const int top = f.messageAlert ? 70 : messageTop(s, int(lines.size())) + 10, text = f.messageAlert ? 90 : top + 29;
    s.drawCentered(c, f.messageTitle, float(W / 2), float(top), 0xFFFFFFFF);
    for (size_t i = 0; i < lines.size(); ++i) s.drawCentered(c, lines[i], float(W / 2), float(text + int(i) * 9), 0xFFFFFFFF);
    s.drawButtons(c);
}

// ConfirmScreen: a column spaced 8 (title, the message of at most 15 lines, two 150 wide buttons 4 apart below 16 more)
// centred on the screen; Esc answers no.
int confirmLines(State& s) { return std::min(15, int(s.font.split(s.fe->confirmText, s.guiW() - 50).size())); }
int confirmTop(State& s) { return (s.guiH() - (9 + 8 + confirmLines(s) * 9 + 8 + 16 + 20)) / 2; }

void buildConfirm(State& s) {
    FrontEndState& f = *s.fe;
    const int W = s.guiW();
    State* p = &s;
    const float y = float(confirmTop(s) + 9 + 8 + confirmLines(s) * 9 + 8 + 16), x = float((W - 304) / 2);
    addButton(s, x, y, 150, Text(f.confirmYes), [p] {
        auto fn = p->fe->confirmAction;
        if (fn) fn();
    });
    auto back = [p] {
        if (auto fn = p->fe->confirmCancel) fn();
        else p->openScreen(p->fe->confirmBack);
    };
    addButton(s, x + 154, y, 150, f.confirmNo.empty() ? tr(s, "gui.cancel") : Text(f.confirmNo), back);
    f.onBack = back;
}

void drawConfirm(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth();
    menuBackground(s, c);
    const int top = confirmTop(s);
    s.drawCentered(c, f.confirmTitle, float(W / 2), float(top), 0xFFFFFFFF);
    const std::vector<Text> lines = s.font.split(f.confirmText, W - 50);
    for (int i = 0; i < confirmLines(s); ++i) s.drawCentered(c, lines[size_t(i)], float(W / 2), float(top + 17 + i * 9), 0xFFFFFFFF);
    s.drawButtons(c);
}

}  // namespace

// --- helpers ------------------------------------------------------------------------------------------------------

Text tr(State& s, const char* key) { return Text(s.res.translate(key)); }
Text trf(State& s, const char* key, const std::vector<Text>& args) { return translatable(key, args, s.lang()); }

std::wstring widen(const std::string& utf8) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), w.data(), int(w.size()));
    return w;
}

std::string utf8(const fs::path& p) {
    const std::wstring& w = p.native();
    std::string s(size_t(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    if (!s.empty()) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
    return s;
}

std::string fitText(Font& f, const std::string& text, int maxWidth) {
    if (f.width(text) <= maxWidth) return text;
    std::string s = text;
    while (!s.empty() && f.width(s + "...") > maxWidth) {
        s.pop_back();
        while (!s.empty() && (u8(s.back()) & 0xC0) == 0x80) s.pop_back();
    }
    return s + "...";
}

void menuBackground(State& s, Canvas& c) {
    // Screen.extractBackground: extractBlurredBackground (with Menu Background Blur at 1 or more, what is drawn so far
    // is blurred: the panorama, or the world and the HUD), then the menu background texture every 32 GUI pixels.
    if (s.settings.menuBackgroundBlurriness >= 1) c.markBlur();
    const Sprite* t = s.sprites.texture(s.fe->cfg.menu ? "gui/menu_background" : "gui/inworld_menu_background");
    tileTexture(c, t, 0, 0, float(c.guiWidth()), float(c.guiHeight()), 32);
}

void listBackground(State& s, Canvas& c, float top, float bottom) {
    const bool menu = s.fe->cfg.menu;
    tileTexture(c, s.sprites.texture(menu ? "gui/menu_list_background" : "gui/inworld_menu_list_background"), 0, top,
                float(c.guiWidth()), bottom, 32);
    const Sprite* header = s.sprites.texture(menu ? "gui/header_separator" : "gui/inworld_header_separator");
    const Sprite* footer = s.sprites.texture(menu ? "gui/footer_separator" : "gui/inworld_footer_separator");
    for (float x = 0; x < float(c.guiWidth()); x += 32) {
        const float w = std::min(32.0f, float(c.guiWidth()) - x);
        blitTexture(c, header, x, top - 2, 0, 0, w, 2, 32, 2);
        blitTexture(c, footer, x, bottom, 0, 0, w, 2, 32, 2);
    }
}

void setFields(State& s, std::vector<Field*> fields, int focus) {
    s.fe->fields = std::move(fields);
    s.fe->focus = focus;
}

void drawFields(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    for (size_t i = 0; i < f.fields.size(); ++i) {
        Field& fl = *f.fields[i];
        const bool focused = int(i) == f.focus;
        const bool hover = s.mouseX >= fl.x && s.mouseY >= fl.y && s.mouseX < fl.x + fl.w && s.mouseY < fl.y + fl.h;
        blitSprite(c, s.sprites.sprite(focused || hover ? "widget/text_field_highlighted" : "widget/text_field"), fl.x, fl.y,
                   fl.w, fl.h);
        const float ty = fl.y + (fl.h - 8) / 2;
        if (fl.text.value.empty() && !focused && !fl.hint.empty()) {
            s.drawText(c, Text(fitText(s.font, fl.hint, int(fl.w) - 8)), fl.x + 4, ty, 0xFF808080, true);
            continue;
        }
        c.pushScissor(fl.x + 2, fl.y, fl.x + fl.w - 2, fl.y + fl.h);
        s.drawTextField(c, fl.text, fl.x + 4, ty, fl.w - 8, fl.textColor, focused, true);
        c.popScissor();
    }
}

bool fieldsInput(State& s, const Window::InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *s.fe;
    if (e.type == E::Button && e.down && e.code == 0) {
        for (size_t i = 0; i < f.fields.size(); ++i) {
            Field& fl = *f.fields[i];
            if (s.mouseX < fl.x || s.mouseY < fl.y || s.mouseX >= fl.x + fl.w || s.mouseY >= fl.y + fl.h) continue;
            f.focus = int(i);
            s.textFieldMouseDown(fl.text);  // EditBox.onClick
            return true;
        }
        f.focus = -1;
        return false;
    }
    if (e.type == E::Key && e.down && e.code == VK_TAB && !f.fields.empty()) {
        const bool back = s.window->keyDown(VK_SHIFT);
        f.focus = (std::max(f.focus, 0) + (back ? int(f.fields.size()) - 1 : 1)) % int(f.fields.size());
        return true;
    }
    if (f.focus < 0 || f.focus >= int(f.fields.size())) return false;
    if (e.type == E::Char || (e.type == E::Key && e.down)) return s.textFieldKey(f.fields[size_t(f.focus)]->text, e);
    return false;
}

Button& addButton(State& s, float x, float y, float w, Text label, std::function<void()> fn, bool active) {
    Button b;
    b.x = x, b.y = y, b.w = w, b.label = std::move(label), b.onPress = std::move(fn), b.active = active;
    s.buttons.push_back(std::move(b));
    return s.buttons.back();
}

const Sprite* iconSprite(State& s, const std::string& key, const Image& img, const std::string& source) {
    FrontEndState& f = *s.fe;
    if (img.w <= 0 || img.h <= 0) return nullptr;
    auto it = f.icons.find(key);
    if (it != f.icons.end() && f.iconSource[key] == source) return &it->second;
    int x, y;
    if (it != f.icons.end() && it->second.w == img.w && it->second.h == img.h) {  // same slot, new pixels
        x = int(std::lround(it->second.u0 * float(s.guiAtlas.width()))), y = int(std::lround(it->second.v0 * float(s.guiAtlas.height())));
    } else if (!s.guiAtlas.alloc(img.w, img.h, x, y)) {
        return nullptr;
    }
    s.guiAtlas.blit(x, y, img.w, img.h, img.rgba.data(), img.w * 4);
    Sprite sp;
    sp.w = sp.sw = img.w, sp.h = sp.sh = img.h;
    const float W = float(s.guiAtlas.width()), H = float(s.guiAtlas.height());
    sp.u0 = float(x) / W, sp.v0 = float(y) / H, sp.u1 = float(x + img.w) / W, sp.v1 = float(y + img.h) / H;
    f.icons[key] = sp;
    f.iconSource[key] = source;
    return &f.icons[key];
}

bool doubleClicked(State& s, int row) {
    FrontEndState& f = *s.fe;
    const bool dbl = row == f.lastClickRow && s.clock - f.lastClickTime < 0.25;
    f.lastClickRow = row, f.lastClickTime = s.clock;
    return dbl;
}

// --- switching instances ------------------------------------------------------------------------------------------

void startChild(State& s, std::vector<std::wstring> args, Text title, Screen back, bool cancellable) {
    FrontEndState& f = *s.fe;
    if (f.child && f.child->state() == launch::Child::State::Running) return;
    if (!f.cfg.childScript.empty()) args.insert(args.end(), {L"--script", f.cfg.childScript});
    f.child = std::make_unique<launch::Child>();
    f.loadingTitle = std::move(title);
    f.loadingStatus.clear();
    f.connectStatus.clear();
    f.progress = 0.02f;
    f.loadingStart = s.clock;
    f.loadingCancel = cancellable;
    f.failBack = back;
    s.openScreen(Screen::Loading);
    if (!f.child->start(args, *s.window)) {
        f.child.reset();
        showMessage(s, Text("Could not start the game"), Text("See the log for details"), s.res.translate("gui.back"),
                    [p = &s, back] { p->openScreen(back); });
    }
}

void showMessage(State& s, Text title, Text text, std::string button, std::function<void()> action, bool alert) {
    FrontEndState& f = *s.fe;
    f.messageTitle = std::move(title), f.messageText = std::move(text), f.messageButton = std::move(button);
    f.messageAction = std::move(action), f.messageAlert = alert;
    s.openScreen(Screen::Message);
}

void showConfirm(State& s, Text title, Text text, std::string yes, std::function<void()> action, Screen back, std::string no,
                 std::function<void()> cancel) {
    FrontEndState& f = *s.fe;
    f.confirmTitle = std::move(title), f.confirmText = std::move(text), f.confirmYes = std::move(yes);
    f.confirmAction = std::move(action), f.confirmBack = back, f.confirmNo = std::move(no), f.confirmCancel = std::move(cancel);
    s.openScreen(Screen::Confirm);
}

void requestTitle(State& s, const std::string& menu) {
    s.fe->titleMenu = menu;
    s.fe->titleRequested = true;
    s.quit = true;
}

void frontEndInit(State& s) {
    FrontEndState& f = *s.fe;
    if (f.cfg.lanPort) f.lanOpenPort = f.cfg.lanPort;
    if (!f.cfg.menu) return;
    f.splash = pickSplash(s.res, f.cfg.playerName);
    f.titleFading = f.cfg.openScreen.empty();  // a fresh start (returning from a world passes --menu title / a list)
    std::mt19937 rng(std::random_device{}());
    f.minceraft = std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < 1.0E-4f;
    // The title screen is the first screen: no cursor recentring (openScreen does that when leaving the game).
    s.screen = Screen::Title;
    s.window->setClickCaptures(false);
    s.buildButtons();
    if (f.cfg.openScreen == "worlds") openWorlds(s);
    else if (f.cfg.openScreen == "servers") openServers(s);
    else if (f.cfg.openScreen == "friends") openFriendsScreen(s);
    else if (f.cfg.openScreen == "create") openWorlds(s), openCreateWorld(s);  // world_create.cpp (tests)
    else friendsMenuCommand(s, f.cfg.openScreen);  // friend-join:<code> (friends_screen.cpp)
    // Gui.addInitialScreens: the accessibility onboarding first, until it is finished once.
    if (f.cfg.openScreen.empty() && s.settings.onboardAccessibility) s.openScreen(Screen::AccessibilityOnboarding);
}

// --- title screen (TitleScreen) -----------------------------------------------------------------------------------

void buildTitle(State& s) {
    const int W = s.guiW(), H = s.guiH();
    State* p = &s;
    const float x = float(W / 2 - 100);
    const int y = H / 4 + 48;
    addButton(s, x, float(y), 200, tr(s, "menu.singleplayer"), [p] { openWorlds(*p); });
    addButton(s, x, float(y + 24), 200, tr(s, "menu.multiplayer"), [p] {  // SafetyScreen first unless skipped
        if (p->settings.skipMultiplayerWarning) openServers(*p);
        else p->openScreen(Screen::MultiplayerWarning);
    });
    // Realms (menu.online) needs a Microsoft account: its place opens the mods and packs instead.
    addButton(s, x, float(y + 48), 200, Text("Mods & Packs"), [p] { p->openScreen(Screen::Mods); });
    // CommonButtons.friends / language / accessibility: three icon buttons centred under them.
    auto icon = [&](int i, const char* sprite, const char* key, const char* tooltip, std::function<void()> fn) -> Button& {
        Button& b = addButton(s, float(W / 2 - (3 * 20 + 2 * 4) / 2 + i * 24), float(y + 72), 20, tr(s, key), std::move(fn));
        b.icon = sprite, b.iconW = b.iconH = 15, b.iconOnly = true;
        b.tooltip = [p, tooltip] { return Text(p->res.translate(tooltip)); };
        return b;
    };
    icon(0, "friends/friends", "gui.friends.open", "gui.friends.open.tooltip", [p] { openFriendsScreen(*p); }).badge = [] {
        return friendsPendingRequests();
    };
    icon(1, "icon/language", "options.language", "options.language.tooltip", [p] {
        p->openScreen(Screen::Language);
        p->optionsParent = Screen::Title;
    });
    icon(2, "icon/accessibility", "options.accessibility", "options.accessibility.tooltip", [p] {
        p->openScreen(Screen::Accessibility);
        p->optionsParent = Screen::Title;
    });
    addButton(s, x, float(y + 96), 98, tr(s, "menu.options"), [p] { p->openScreen(Screen::Options); });
    addButton(s, float(W / 2 + 2), float(y + 96), 98, tr(s, "menu.quit"), [p] { p->quit = true; });
    s.fe->onBack = nullptr;
}

void drawTitle(State& s, Canvas& c) {
    FrontEndState& f = *s.fe;
    const int W = c.guiWidth(), H = c.guiHeight();
    float widgetFade = 1.0f;  // TitleScreen.extractRenderState: fade = elapsed / 2 s, widgets from half way
    if (f.titleFading) {
        if (f.titleFadeStart < 0) f.titleFadeStart = s.clock;
        const float fade = float(s.clock - f.titleFadeStart) / 2.0f;
        if (fade > 1.0f) f.titleFading = false;
        else widgetFade = std::clamp((std::clamp(fade, 0.0f, 1.0f) - 0.5f) / 0.5f, 0.0f, 1.0f);
    }
    // The widgets first (the buttons and the copyright line), then the logo, the splash and the version.
    c.setAlpha(widgetFade);
    // PlainTextButton: the copyright line, underlined while hovered, opens Credits and Attribution.
    Text credits(s.res.translate("title.credits"));
    const int cw = s.font.width(credits);
    if (s.mouseX >= W - cw - 2 && s.mouseX < W - 2 && s.mouseY >= H - 10 && s.mouseY < H) {
        Style underlined;
        underlined.underlined = 1;
        credits = credits.withStyle(underlined);
    }
    s.drawText(c, credits, float(W - cw - 2), float(H - 10), 0xFFFFFFFF, true);
    s.drawButtons(c);
    // LogoRenderer: 256x44 of the 256x64 logo, the edition 7 pixels into it.
    c.setAlpha(f.keepLogoThroughFade ? 1.0f : widgetFade);
    blitTexture(c, s.sprites.texture(f.minceraft ? "gui/title/minceraft" : "gui/title/minecraft"), float(W / 2 - 128), 30, 0, 0, 256, 44,
                256, 64);
    blitTexture(c, s.sprites.texture("gui/title/edition"), float(W / 2 - 64), 30 + 44 - 7, 0, 0, 128, 14, 128, 16);
    c.setAlpha(widgetFade);
    if (!f.splash.empty() && !s.settings.hideSplashTexts) {  // SplashRenderer (TitleScreen: unless Hide Splash Texts)
        const Text splash(f.splash);
        const int textW = s.font.width(splash);
        const double ms = std::fmod(s.clock * 1000.0, 1000.0);
        const float phase = 1.8f - std::abs(std::sin(float(ms / 1000.0) * kPi * 2)) * 0.1f;
        c.push();
        c.translate(float(W) / 2 + 123, 69);
        c.rotate(-kPi / 9);
        c.scaleBy(phase * 100.0f / float(textW + 32));
        s.drawText(c, splash, float(-textW / 2), -8, 0xFFFFFF00, true);
        c.pop();
    }
    s.drawText(c, Text("Minecraft " + f.cfg.version + " (minecraftCplusplus)"), 2, float(H - 10), 0xFFFFFFFF, true);
    c.setAlpha(1.0f);
}

// --- dispatch -----------------------------------------------------------------------------------------------------

void Ui::State::buildFrontEndButtons() {
    FrontEndState& f = *fe;
    const std::vector<Field*> prevFields = std::move(f.fields);
    const int prevFocus = f.focus;
    f.fields.clear();
    f.focus = -1;
    f.onBack = nullptr, f.onEnter = nullptr;
    switch (screen) {
        case Screen::Title: buildTitle(*this); break;
        case Screen::Worlds: buildWorlds(*this); break;
        case Screen::CreateWorld: buildCreateWorld(*this); break;
        case Screen::EditWorld: buildEditWorld(*this); break;
        case Screen::Confirm: buildConfirm(*this); break;
        case Screen::Servers: buildServers(*this); break;
        case Screen::EditServer: buildEditServer(*this); break;
        case Screen::DirectConnect: buildDirectConnect(*this); break;
        case Screen::Loading: buildLoading(*this); break;
        case Screen::Message: buildMessage(*this); break;
        default: break;
    }
    if (f.fields == prevFields) f.focus = prevFocus;  // a re-layout (window resized) keeps the focus
}

void Ui::State::drawFrontEnd(Canvas& c) {
    switch (screen) {
        case Screen::Title: drawTitle(*this, c); break;
        case Screen::Worlds: drawWorlds(*this, c); break;
        case Screen::CreateWorld: drawCreateWorld(*this, c); break;
        case Screen::EditWorld: drawEditWorld(*this, c); break;
        case Screen::Confirm: drawConfirm(*this, c); break;
        case Screen::Servers: drawServers(*this, c); break;
        case Screen::EditServer: drawEditServer(*this, c); break;
        case Screen::DirectConnect: drawDirectConnect(*this, c); break;
        case Screen::Loading: drawLoading(*this, c); break;
        case Screen::Message: drawMessage(*this, c); break;
        default: break;
    }
}

void Ui::State::frontEndInput(const Window::InputEvent& e) {
    using E = Window::InputEvent;
    FrontEndState& f = *fe;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) {
        if (auto fn = f.onBack) fn();
        return;
    }
    if (e.type == E::Key && e.down && e.code == VK_RETURN) {
        if (auto fn = f.onEnter) fn();
        return;
    }
    if (screen == Screen::Worlds) return worldsInput(*this, e);
    if (screen == Screen::Servers) return serversInput(*this, e);
    if (screen == Screen::CreateWorld) return createWorldInput(*this, e);
    if (screen == Screen::EditWorld) return editWorldInput(*this, e);
    if (screen == Screen::Title && e.type == E::Button && e.down && e.code == 0) {  // the credits line (PlainTextButton)
        const int W = guiW(), H = guiH(), cw = font.width(Text(res.translate("title.credits")));
        if (mouseX >= W - cw - 2 && mouseX < W - 2 && mouseY >= H - 10 && mouseY < H) {
            audio::playUiClick();
            openScreen(Screen::CreditsAttribution);
            optionsParent = Screen::Title;
            return;
        }
    }
    if (fieldsInput(*this, e)) return;
    buttonsInput(e);
}

void Ui::State::frontEndFrame() {
    FrontEndState& f = *fe;
    if (f.child) {
        const bool connecting = connectScreen(f);
        for (const std::string& line : f.child->takeLines()) loadingLine(*this, line);
        if (connecting != connectScreen(f) && screen == Screen::Loading) buildButtons();  // on to the terrain
        const launch::Child::State st = f.child->state();
        if (st == launch::Child::State::Ready && !f.handedOff) {
            // The next instance shows its window over this one: go.
            f.handedOff = true;
            quit = true;
            ShowWindow(window->hwnd(), SW_HIDE);
            LOG("launch: handed over after %.2f s", clock - f.loadingStart);
        } else if (st == launch::Child::State::Failed) {
            if (f.leaving) {  // the title screen did not start: just close
                quit = true;
                f.handedOff = true;
            } else {
                std::string error = f.child->error();
                f.child.reset();
                const Screen back = f.failBack;
                std::function<void()> action = std::move(f.failAction);
                if (error.empty()) error = "It closed while starting; see the log for details.";
                showMessage(*this, f.failTitle.empty() ? Text("Could not open the world") : f.failTitle, Text(error),
                            res.translate(back == Screen::Servers ? "gui.toMenu" : "gui.back"),
                            action ? action : [this, back] { back == Screen::Servers ? openServers(*this) : back == Screen::Worlds ? openWorlds(*this) : openScreen(back); });
            }
        }
    }
    serversFrame(*this);
    worldsFrame(*this);
    // In a world: the server closed the connection (kicked, stopped, crashed).
    if (!f.cfg.menu && session && !session->connected() && !f.leaving && !f.titleRequested && screen != Screen::Message &&
        !f.child) {  // (a transfer: the next instance is connecting)
        const bool multiplayer = !f.cfg.singleplayer;
        std::string reason = session->disconnectReason();
        if (reason.empty()) reason = res.translate("disconnect.endOfStream");
        LOG("ui: disconnected: %s", reason.c_str());
        clearToasts();  // Hud.onDisconnected: ToastManager.clear
        showMessage(*this, Text(res.translate("disconnect.lost")), Text(reason),
                    res.translate(multiplayer ? "gui.toMenu" : "gui.toTitle"),
                    [this, multiplayer] { requestTitle(*this, multiplayer ? "servers" : ""); });
    }
}

// --- Ui -----------------------------------------------------------------------------------------------------------

void Ui::setFrontEnd(const FrontEnd& fe) { s_->fe->cfg = fe; }
bool Ui::titleRequested() const { return s_->fe->titleRequested; }
bool Ui::switching() const { return s_->fe->child && s_->fe->child->state() == launch::Child::State::Running; }

void Ui::showSaving() {
    State& s = *s_;
    FrontEndState& f = *s.fe;
    f.leaving = true;
    s.quit = false;
    f.loadingTitle = tr(s, "menu.savingLevel");
    f.loadingStatus.clear();
    f.progress = -1;
    f.loadingCancel = false;
    f.loadingStart = s.clock;
    s.openScreen(Screen::Loading);
}

void Ui::startTitle() {
    State& s = *s_;
    FrontEndState& f = *s.fe;
    std::vector<std::wstring> args = launch::stickyArguments();
    // the list to open, or the plain title screen (no fade-in: not the game's first title screen)
    args.insert(args.end(), {L"--menu", f.titleMenu.empty() ? std::wstring(L"title") : widen(f.titleMenu)});
    f.child = std::make_unique<launch::Child>();
    f.loadingStart = s.clock;
    if (!f.child->start(args, *s.window)) {
        f.child.reset();
        s.quit = true;  // nothing to hand over to: close
    }
}

}  // namespace ui
