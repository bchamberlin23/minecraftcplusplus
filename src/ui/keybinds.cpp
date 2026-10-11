// Key mappings: the game's 53 named mappings and the 9 hotbar slots (Options.keyMappings) with their default keys,
// the InputConstants key names options.txt stores (key_key.forward:key.keyboard.w) and how bound keys are shown.
#include "ui/ui_state.h"

namespace ui {

namespace {

// In Key order. Defaults are the game's (InputConstants scancodes, US layout) as virtual keys; 0 = unbound.
const KeyDef kKeys[kKeyCount] = {
    {"key.attack", "gameplay", VK_LBUTTON},
    {"key.use", "gameplay", VK_RBUTTON},
    {"key.forward", "movement", 'W'},
    {"key.left", "movement", 'A'},
    {"key.back", "movement", 'S'},
    {"key.right", "movement", 'D'},
    {"key.jump", "movement", VK_SPACE},
    {"key.sneak", "movement", VK_LSHIFT},
    {"key.sprint", "movement", VK_LCONTROL},
    {"key.drop", "inventory", 'Q'},
    {"key.inventory", "inventory", 'E'},
    {"key.chat", "multiplayer", 'T'},
    {"key.playerlist", "multiplayer", VK_TAB},
    {"key.pickItem", "gameplay", VK_MBUTTON},
    {"key.command", "multiplayer", VK_OEM_2},
    {"key.friends", "multiplayer", 'O'},
    {"key.socialInteractions", "multiplayer", 'P'},
    {"key.toggleGui", "misc", VK_F1},
    {"key.toggleSpectatorShaderEffects", "misc", VK_F4},
    {"key.screenshot", "misc", VK_F2},
    {"key.togglePerspective", "misc", VK_F5},
    {"key.smoothCamera", "misc", 0},
    {"key.fullscreen", "misc", VK_F11},
    {"key.spectatorOutlines", "spectator", 0},
    {"key.spectatorHotbar", "spectator", VK_MBUTTON},
    {"key.swapOffhand", "inventory", 'F'},
    {"key.saveToolbarActivator", "creative", 'C'},
    {"key.loadToolbarActivator", "creative", 'X'},
    {"key.advancements", "misc", 'L'},
    {"key.quickActions", "misc", 'G'},
    {"key.debug.overlay", "debug", VK_F3, -2},
    {"key.debug.modifier", "debug", VK_F3, -1},
    {"key.hotbar.1", "inventory", '1'},
    {"key.hotbar.2", "inventory", '2'},
    {"key.hotbar.3", "inventory", '3'},
    {"key.hotbar.4", "inventory", '4'},
    {"key.hotbar.5", "inventory", '5'},
    {"key.hotbar.6", "inventory", '6'},
    {"key.hotbar.7", "inventory", '7'},
    {"key.hotbar.8", "inventory", '8'},
    {"key.hotbar.9", "inventory", '9'},
    {"key.debug.reloadChunk", "debug", 'A'},
    {"key.debug.showHitboxes", "debug", 'B'},
    {"key.debug.clearChat", "debug", 'D'},
    {"key.debug.crash", "debug", 'C'},
    {"key.debug.showChunkBorders", "debug", 'G'},
    {"key.debug.showAdvancedTooltips", "debug", 'H'},
    {"key.debug.copyRecreateCommand", "debug", 'I'},
    {"key.debug.spectate", "debug", 'N'},
    {"key.debug.switchGameMode", "debug", VK_F4},
    {"key.debug.debugOptions", "debug", VK_F6},
    {"key.debug.focusPause", "debug", 'P'},
    {"key.debug.dumpDynamicTextures", "debug", 'S'},
    {"key.debug.reloadResourcePacks", "debug", 'T'},
    {"key.debug.profiling", "debug", 'L'},
    {"key.debug.copyLocation", "debug", 'C'},
    {"key.debug.dumpVersion", "debug", 'V'},
    {"key.debug.profilingChart", "debug", '1', 1},
    {"key.debug.fpsCharts", "debug", '2', 2},
    {"key.debug.networkCharts", "debug", '3', 3},
    {"key.debug.lightmapTexture", "debug", '4', 4},
    {"key.debug.improvedTransparency", "debug", 'X'},
};

// InputConstants names of the keys Windows reports (letters and digits are added below).
const std::pair<int, const char*> kNames[] = {
    {VK_LBUTTON, "key.mouse.left"},
    {VK_RBUTTON, "key.mouse.right"},
    {VK_MBUTTON, "key.mouse.middle"},
    {VK_XBUTTON1, "key.mouse.4"},
    {VK_XBUTTON2, "key.mouse.5"},
    {VK_RETURN, "key.keyboard.enter"},
    {VK_ESCAPE, "key.keyboard.escape"},
    {VK_BACK, "key.keyboard.backspace"},
    {VK_TAB, "key.keyboard.tab"},
    {VK_SPACE, "key.keyboard.space"},
    {VK_OEM_MINUS, "key.keyboard.minus"},
    {VK_OEM_PLUS, "key.keyboard.equal"},
    {VK_OEM_4, "key.keyboard.left.bracket"},
    {VK_OEM_6, "key.keyboard.right.bracket"},
    {VK_OEM_5, "key.keyboard.backslash"},
    {VK_OEM_102, "key.keyboard.world.2"},
    {VK_OEM_1, "key.keyboard.semicolon"},
    {VK_OEM_7, "key.keyboard.apostrophe"},
    {VK_OEM_3, "key.keyboard.grave.accent"},
    {VK_OEM_COMMA, "key.keyboard.comma"},
    {VK_OEM_PERIOD, "key.keyboard.period"},
    {VK_OEM_2, "key.keyboard.slash"},
    {VK_CAPITAL, "key.keyboard.caps.lock"},
    {VK_SNAPSHOT, "key.keyboard.print.screen"},
    {VK_SCROLL, "key.keyboard.scroll.lock"},
    {VK_PAUSE, "key.keyboard.pause"},
    {VK_INSERT, "key.keyboard.insert"},
    {VK_HOME, "key.keyboard.home"},
    {VK_PRIOR, "key.keyboard.page.up"},
    {VK_DELETE, "key.keyboard.delete"},
    {VK_END, "key.keyboard.end"},
    {VK_NEXT, "key.keyboard.page.down"},
    {VK_RIGHT, "key.keyboard.right"},
    {VK_LEFT, "key.keyboard.left"},
    {VK_DOWN, "key.keyboard.down"},
    {VK_UP, "key.keyboard.up"},
    {VK_NUMLOCK, "key.keyboard.num.lock"},
    {VK_DIVIDE, "key.keyboard.keypad.divide"},
    {VK_MULTIPLY, "key.keyboard.keypad.multiply"},
    {VK_SUBTRACT, "key.keyboard.keypad.subtract"},
    {VK_ADD, "key.keyboard.keypad.add"},
    {VK_DECIMAL, "key.keyboard.keypad.decimal"},
    {VK_APPS, "key.keyboard.menu"},
    {VK_LCONTROL, "key.keyboard.left.control"},
    {VK_LSHIFT, "key.keyboard.left.shift"},
    {VK_LMENU, "key.keyboard.left.alt"},
    {VK_LWIN, "key.keyboard.left.win"},
    {VK_RCONTROL, "key.keyboard.right.control"},
    {VK_RSHIFT, "key.keyboard.right.shift"},
    {VK_RMENU, "key.keyboard.right.alt"},
    {VK_RWIN, "key.keyboard.right.win"},
};

}  // namespace

const KeyDef& keyDef(Key k) { return kKeys[size_t(k)]; }

std::array<int, kKeyCount> Settings::defaultKeys() {
    std::array<int, kKeyCount> keys{};
    for (size_t i = 0; i < kKeyCount; ++i) keys[i] = kKeys[i].defaultKey;
    return keys;
}

std::string keyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string("key.keyboard.") + char(std::tolower(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return "key.keyboard.f" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "key.keyboard.keypad." + std::to_string(vk - VK_NUMPAD0);
    for (auto [k, n] : kNames)
        if (k == vk) return n;
    return "key.keyboard.unknown";
}

int keyFromName(std::string_view name) {
    for (int vk = 1; vk < 256; ++vk)
        if (name == keyName(vk)) return vk;
    return 0;
}

// InputConstants.Key.getDisplayName: a key that types one character shows it upper-cased (keyboard layout aware),
// other keys their translated name; mouse buttons without a translation "Button <n>".
Text Ui::State::keyLabel(int vk) const {
    const std::string name = keyName(vk);
    if (name == "key.keyboard.unknown") return Text(res.translate(name.c_str()));
    if (name.starts_with("key.mouse.")) {
        const std::string t = res.translate(name.c_str());
        return Text(t != name ? t : formatJava(res.translate("key.mouse"), {name.substr(10)}));
    }
    const UINT ch = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFF;
    if (ch > 32 && !(vk >= VK_NUMPAD0 && vk <= VK_DIVIDE)) {
        wchar_t w = wchar_t(ch);
        CharUpperBuffW(&w, 1);
        char buf[8] = {};
        WideCharToMultiByte(CP_UTF8, 0, &w, 1, buf, sizeof(buf) - 1, nullptr, nullptr);
        return Text(buf);
    }
    return Text(res.translate(name.c_str()));
}

bool Ui::State::isKey(Key k, const Window::InputEvent& e) const {
    const int bound = settings.key(k);
    if (!bound || !e.down) return false;
    if (e.type == Window::InputEvent::Key) return e.code == bound;
    if (e.type == Window::InputEvent::Button) {
        static const int kButtons[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        return e.code >= 0 && e.code < 5 && kButtons[e.code] == bound;
    }
    return false;
}

}  // namespace ui
