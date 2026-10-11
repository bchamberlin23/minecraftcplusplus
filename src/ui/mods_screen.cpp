// Mods & Packs screen (from the pause menu): everything in the mods folder and the vanilla resource packs, with
// on/off, order (packs: top of the list wins), the mods folder, and a restart to apply changes. Also the UI side of
// native mods (mods/native_mods.h): host callbacks, block / chat events and their HUD.
#include "assets/assets.h"
#include "mods/mod_manager.h"
#include "ui/frontend.h"
#include "ui/ui_state.h"
#include "world/world.h"

#include <cmath>

namespace ui {

namespace {

constexpr int kRowH = 24, kListW = 360, kTop = 34;
constexpr float kDeg = 57.2957795f;

int listBottom(int H) { return H - 60; }

std::string fit(Font& f, std::string s, int maxW) {
    if (f.width(s) <= maxW) return s;
    while (!s.empty() && f.width(s + "...") > maxW) {
        s.pop_back();
        while (!s.empty() && (u8(s.back()) & 0xC0) == 0x80) s.pop_back();  // whole UTF-8 characters
    }
    return s + "...";
}

u32 kindColor(mods::Kind k) {
    switch (k) {
    case mods::Kind::ResourcePack: return 0xFF55FFFF;
    case mods::Kind::DataPack: return 0xFF55FF55;
    case mods::Kind::ResourceAndDataPack: return 0xFF7FD4FF;
    case mods::Kind::FabricMod: return 0xFFFFC266;
    case mods::Kind::NativeMod: return 0xFFFF77FF;
    default: return 0xFFFF5555;
    }
}

const char* kindTag(mods::Kind k) {
    switch (k) {
    case mods::Kind::ResourcePack: return "[Resource]";
    case mods::Kind::DataPack: return "[Data]";
    case mods::Kind::ResourceAndDataPack: return "[Res+Data]";
    case mods::Kind::FabricMod: return "[Fabric]";
    case mods::Kind::NativeMod: return "[Native]";
    default: return "[Unsupported]";
    }
}

bool movable(const mods::Entry& e) { return e.isResourcePack() || e.isDataPack(); }

}  // namespace

void Ui::State::initNativeMods() {
    mods::Manager& mm = mods::Manager::get();
    if (!mm.initialized()) return;
    // Block events for native mods; audio chains its own hook after this one.
    game->onBlockChanged = [this, prev = std::move(game->onBlockChanged)](bool place, int x, int y, int z, u16 state) {
        if (prev) prev(place, x, y, z, state);
        nativeMods.blockChanged(place, x, y, z, registry->name(state));
    };
    const std::vector<fs::path> dlls = mm.nativeMods();
    if (dlls.empty()) return;
    mods::NativeMods::Host host;
    host.chat = [this](const std::string& t, u32 argb) { addChat(Text(t, colorStyle(argb & 0xFFFFFF))); };
    host.sendChat = [this](const std::string& t) { sendChat(t); };
    host.block = [this](int x, int y, int z) { return registry->name(world->block(x, y, z)); };
    host.position = [this](double o[3]) {
        const Player& p = game->player();
        o[0] = p.pos[0], o[1] = p.pos[1], o[2] = p.pos[2];
    };
    host.rotation = [this](float o[2]) {
        const Player& p = game->player();
        o[0] = p.yaw * kDeg, o[1] = p.pitch * kDeg;
    };
    host.online = [this] { return online(); };
    nativeMods.load(dlls, mm.modsDir(), std::move(host));
}

void Ui::State::drawModHud(Canvas& c) {
    if (!nativeMods.activeCount()) return;
    mods::NativeMods::Hud hud;
    hud.width = float(c.guiWidth()), hud.height = float(c.guiHeight());
    hud.text = [&](float x, float y, u32 argb, const std::string& t) { drawText(c, Text(t), x, y, argb, true); };
    hud.textWidth = [&](const std::string& t) { return font.width(t); };
    hud.rect = [&](float x0, float y0, float x1, float y1, u32 argb) { c.fill(x0, y0, x1, y1, argb); };
    nativeMods.drawHud(hud);
}

void Ui::State::buildModsButtons() {
    mods::Manager& mm = mods::Manager::get();
    mm.detectPending();
    const int W = canvas ? guiW() : int(window->width()) / std::max(1, guiScale());
    const int H = canvas ? guiH() : int(window->height()) / std::max(1, guiScale());
    const auto& entries = mm.entries();
    const int rows = std::max(1, (listBottom(H) - kTop) / kRowH);
    modsScroll = std::clamp(modsScroll, 0, std::max(0, int(entries.size()) - rows));
    const int x1 = W / 2 + kListW / 2;
    for (int r = 0; r < rows && modsScroll + r < int(entries.size()); ++r) {
        const size_t i = size_t(modsScroll + r);
        const mods::Entry& e = entries[i];
        const float y = float(kTop + r * kRowH + 2);
        if (e.kind == mods::Kind::Unsupported) continue;
        Button on;
        on.x = float(x1 - 36), on.y = y, on.w = 34;
        on.label = Text(e.enabled ? "On" : "Off", colorStyle(e.enabled ? 0x55FF55 : 0xFF5555));
        on.onPress = [this, i] {
            mods::Manager& m = mods::Manager::get();
            m.setEnabled(i, !m.entries()[i].enabled);
            buildButtons();
        };
        buttons.push_back(std::move(on));
        if (!movable(e)) continue;
        for (int d : {-1, 1}) {
            const bool can = i + d < entries.size() && int(i) + d >= 0 && movable(entries[i + d]);
            Button mv;
            mv.x = float(x1 - 36 - (d < 0 ? 44 : 22)), mv.y = y, mv.w = 20;
            mv.label = Text(d < 0 ? "\xE2\x96\xB2" : "\xE2\x96\xBC");  // up / down triangles
            mv.active = can;
            mv.onPress = [this, i, d] {
                mods::Manager::get().move(i, d);
                buildButtons();
            };
            buttons.push_back(std::move(mv));
        }
    }
    // Bottom row.
    const float by = float(H - 28);
    auto add = [&](float x, float w, Text label, std::function<void()> fn, bool active = true) {
        Button b;
        b.x = x, b.y = by, b.w = w, b.label = std::move(label), b.onPress = std::move(fn), b.active = active;
        buttons.push_back(std::move(b));
    };
    const float bx = float(W / 2 - 182);
    add(bx, 110, Text("Open Mods Folder"), [] { mods::Manager::get().openModsFolder(); });
    add(bx + 114, 70, Text("Refresh"), [this] {
        mods::Manager::get().rescan();
        buildButtons();
    });
    add(bx + 188, 100, Text("Restart to Apply"), [this] {
        mods::Manager::get().requestRestart();
        quit = true;
    }, mm.restartNeeded());
    add(bx + 292, 72, Text(res.translate("gui.done")), [this] { modsBack(); });
}

void Ui::State::drawModsScreen(Canvas& c) {
    mods::Manager& mm = mods::Manager::get();
    const int W = c.guiWidth(), H = c.guiHeight();
    c.gradient(0, 0, float(W), float(H), 0xC0101010, 0xD0101010);
    drawCentered(c, Text("Mods & Packs"), float(W / 2), 8, 0xFFFFFFFF);
    drawCentered(c, Text(fit(font, "Drop packs (.zip / folders), Fabric mods (.jar) and native mods (.dll) into the mods folder", W - 8)),
                 float(W / 2), 20, 0xFFA0A0A0);
    const auto& entries = mm.entries();
    const int x0 = W / 2 - kListW / 2, x1 = W / 2 + kListW / 2;
    const int rows = std::max(1, (listBottom(H) - kTop) / kRowH);
    c.fill(float(x0 - 2), float(kTop - 2), float(x1 + 2), float(kTop + rows * kRowH + 2), 0x60000000);
    if (entries.empty())
        drawCentered(c, Text("The mods folder is empty"), float(W / 2), float(kTop + 8), 0xFFA0A0A0);
    int hovered = -1;
    for (int r = 0; r < rows && modsScroll + r < int(entries.size()); ++r) {
        const mods::Entry& e = entries[size_t(modsScroll + r)];
        const float y = float(kTop + r * kRowH);
        if (mouseX >= float(x0) && mouseX < float(x1) && mouseY >= y && mouseY < y + kRowH) {
            hovered = modsScroll + r;
            c.fill(float(x0), y, float(x1), y + kRowH, 0x30FFFFFF);
        }
        const int textW = x1 - x0 - (movable(e) ? 88 : e.kind == mods::Kind::Unsupported ? 6 : 44);
        const std::string tag = kindTag(e.kind);
        drawText(c, Text(tag), float(x0 + 3), y + 3, kindColor(e.kind), true);
        const int tagW = font.width(tag) + 4;
        drawText(c, Text(fit(font, e.name, textW - tagW)), float(x0 + 3 + tagW), y + 3, e.enabled ? 0xFFFFFFFF : 0xFF909090, true);
        // Second line: what matters most (a warning, the native mod's state, the pack's version note, its description).
        std::string detail = e.description;
        u32 color = 0xFF909090;
        if (e.kind == mods::Kind::NativeMod)
            for (const auto& st : nativeMods.status())
                if (st.file == e.path) detail = (st.active ? "running: " : "") + st.name + " " + st.version + " - " + st.state;
        if (!e.compat.empty()) detail = e.compat + (detail.empty() ? "" : " - " + detail), color = 0xFFFFAA00;
        if (!e.warnings.empty()) detail = e.warnings.front(), color = 0xFFFFFF55;
        if (e.source != "mods folder" && e.warnings.empty()) detail = "(" + e.source + ") " + detail;
        drawText(c, Text(fit(font, detail, textW)), float(x0 + 3), y + 13, color, false);
    }
    if (int(entries.size()) > rows) {  // scroll bar
        const float h = float(rows * kRowH), bar = std::max(8.0f, h * float(rows) / float(entries.size()));
        const float t = (h - bar) * float(modsScroll) / float(std::max(1, int(entries.size()) - rows));
        c.fill(float(x1 + 3), float(kTop) + t, float(x1 + 6), float(kTop) + t + bar, 0xFFA0A0A0);
    }
    if (mm.restartNeeded())
        drawCentered(c, Text("Changes apply after a restart"), float(W / 2), float(H - 44), 0xFFFFFF55);
    else
        drawCentered(c, Text("Folder: " + fit(font, mm.modsDir().string(), 300)), float(W / 2), float(H - 44), 0xFF707070);
    drawButtons(c);
    if (hovered >= 0) {  // everything known about the entry
        const mods::Entry& e = entries[size_t(hovered)];
        std::vector<Text> lines{Text(e.name), Text(std::string(mods::kindName(e.kind)) + " - " + e.source, colorStyle(0xA0A0A0))};
        if (!e.description.empty()) lines.push_back(Text(fit(font, e.description, 300), colorStyle(0xA0A0A0)));
        if (!e.compat.empty()) lines.push_back(Text(e.compat, colorStyle(0xFFAA00)));
        for (const std::string& w : e.warnings)
            for (const Text& l : font.split(Text(w, colorStyle(0xFFFF55)), 300)) lines.push_back(l);
        if (mouseX < float(x1 - 90)) drawTooltip(c, lines, mouseX, mouseY);
    }
}

void Ui::State::modsBack() {
    const Screen to = modsReturn != Screen::None ? modsReturn : fe->cfg.menu ? Screen::Title : Screen::Pause;
    modsReturn = Screen::None;
    openScreen(to);
}

void Ui::State::modsInput(const Window::InputEvent& e) {
    using E = Window::InputEvent;
    if (e.type == E::Key && e.down && e.code == VK_ESCAPE) return modsBack();
    if (e.type == E::Wheel) {
        modsScroll -= e.code > 0 ? 1 : -1;
        buildButtons();
        return;
    }
    buttonsInput(e);
}

}  // namespace ui
