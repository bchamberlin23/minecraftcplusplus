// Sleeping (InBedChatScreen: chat with a "Leave Bed" button while the server keeps the player in bed, and the
// lying-down first-person camera of Camera.setup), the debug key combinations F3 + F4 (GameModeSwitcherScreen) and
// F3 + N (spectator toggle), F3 + H (advanced tooltips), F3 + P (pause on lost focus), F3 + D (clear chat), and F3
// alone toggling the debug overlay on release (KeyboardHandler; all with the bound debug keys).
#include "assets/assets.h"
#include "audio/audio.h"
#include "ui/debug_screen.h"
#include "ui/ui_state.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {
constexpr float kPi = 3.14159265f;

struct Bed {
    bool sleeping = false;       // LivingEntity.isSleeping: the sleeping position is set
    int pos[3] = {};             // the bed block (SLEEPING_POS_ID)
    bool hookSet = false;
};
Bed bed;

struct Switcher {
    bool f3Down = false, usedAsModifier = false;
    int lastMode = -1, previousMode = -1;  // MultiPlayerGameMode.previousPlayerMode
    int hovered = 0;                       // GameModeIcon: creative, survival, adventure, spectator
    bool firstMouseSet = false;
    float firstMouse[2] = {};
};
Switcher sw;

constexpr int kIconMode[4] = {1, 0, 2, 3};  // GameModeIcon -> GameType id
const char* const kIconName[4] = {"gameMode.creative", "gameMode.survival", "gameMode.adventure", "gameMode.spectator"};
const char* const kIconItem[4] = {"grass_block", "iron_sword", "buried_treasure_map", "ender_eye"};

int iconOf(int mode) {
    for (int i = 0; i < 4; ++i)
        if (kIconMode[i] == mode) return i;
    return 1;
}

void sendGameMode(UiState& s, int mode) {  // ServerboundChangeGameModePacket
    if (!s.online() || mode == s.gameMode()) return;
    net::Writer w;
    w.varInt(mode);
    s.client->sendPlay("change_game_mode", w);
}

// --- packets ----------------------------------------------------------------------------------------------------

void handlers(UiState& s, net::Client& client) {
    UiState* sp = &s;
    // Our sleeping position (LivingEntity.SLEEPING_POS_ID, the player's only optional block position) and pose.
    client.onPlayPeek("set_entity_data", [sp](net::Reader& r) {
        const int entity = r.varInt();
        int found = 0;  // 1 sleeping, 2 awake
        i64 packed = 0;
        for (int guard = 0; guard < 64 && r.ok; ++guard) {
            const int index = r.u8v();
            if (index == 0xFF || !r.ok) break;
            const int type = r.varInt();
            bool stop = false;
            switch (type) {
                case 0: r.u8v(); break;
                case 1: case 12: case 14: case 15: case 19: case 20: r.varInt(); break;
                case 2: r.varLong(); break;
                case 3: r.f32(); break;
                case 4: r.str(); break;
                case 5: r.nbt(); break;
                case 6: if (r.boolean()) r.nbt(); break;
                case 8: r.boolean(); break;
                case 9: r.f32(), r.f32(), r.f32(); break;
                case 10: r.i64v(); break;
                case 11:
                    if (r.boolean()) found = 1, packed = r.i64v();
                    else found = 2;
                    break;
                case 13: if (r.boolean()) r.skip(16); break;
                default: stop = true; break;
            }
            (void)index;
            if (stop) break;
        }
        if (!found) return;
        sp->post([sp, entity, found, packed] {
            if (!sp->session || entity != sp->session->entityId()) return;
            bed.sleeping = found == 1;
            if (found == 1) {
                bed.pos[0] = int(packed >> 38), bed.pos[1] = int((packed << 52) >> 52), bed.pos[2] = int((packed << 26) >> 38);
            }
            if (sp->game) {
                sp->game->sleeping = bed.sleeping;
                std::copy(bed.pos, bed.pos + 3, sp->game->bedPos);
            }
        });
    });
    client.onPlayPeek("respawn", [sp](net::Reader&) {
        sp->post([sp] {
            bed.sleeping = false;
            if (sp->game) sp->game->sleeping = false;
        });
    });
}

void attached(UiState& s) {
    bed.sleeping = false;
    if (s.game) s.game->sleeping = false;
    sw.lastMode = sw.previousMode = -1;
}

// Camera.setup for a sleeping entity: at the bed (setPosToBed + the sleeping eye height 0.2), facing the bed's
// orientation reversed, level, then moved 0.15 up.
void sleepingCamera(UiState& s, CameraState& c) {
    if (!bed.sleeping) return;
    if (s.game->cameraMode != 0) {  // detached: from the sleeping eyes (0.2 above the bed) along the player's look
        c.x = bed.pos[0] + 0.5, c.y = bed.pos[1] + 0.6875 + 0.2, c.z = bed.pos[2] + 0.5;
        return;
    }
    const std::string& state = s.registry->name(s.world->block(bed.pos[0], bed.pos[1], bed.pos[2]));
    float bedYaw = 0;  // Direction.toYRot of the bed's facing: south 0, west 90, north 180, east 270
    if (state.find("facing=west") != std::string::npos) bedYaw = 90;
    else if (state.find("facing=north") != std::string::npos) bedYaw = 180;
    else if (state.find("facing=east") != std::string::npos) bedYaw = 270;
    const bool isBed = state.find("_bed") != std::string::npos;
    c.x = bed.pos[0] + 0.5, c.y = bed.pos[1] + 0.6875 + 0.2 + 0.15, c.z = bed.pos[2] + 0.5;
    c.yaw = (isBed ? bedYaw - 180.0f : 0.0f) * kPi / 180.0f;
    c.pitch = 0;
}

void tick(UiState& s) {
    if (!bed.hookSet && s.game) {
        UiState* sp = &s;
        s.game->viewCameraOverride = [sp](CameraState& c) { sleepingCamera(*sp, c); };
        bed.hookSet = true;
    }
    const int mode = s.gameMode();
    if (mode != sw.lastMode) {
        if (sw.lastMode >= 0) sw.previousMode = sw.lastMode;
        sw.lastMode = mode;
    }
    // Minecraft.tick: the bed chat opens while sleeping and closes (keeping a typed message in the chat) on waking.
    if (bed.sleeping && s.screen == Screen::None && s.online()) s.openScreen(Screen::InBed);
    if (!bed.sleeping && s.screen == Screen::InBed) {
        const std::string text = s.chatInput.value;
        s.closeScreen(false);
        if (!text.empty()) {
            s.openScreen(Screen::Chat);
            s.chatInput.value = text;
            s.chatInput.cursor = s.chatInput.selection = int(text.size());
        }
    }
}

}  // namespace

// KeyboardHandler.debugFeedbackTranslated: "[Debug]:" in bold yellow, then the message.
void debugFeedback(UiState& s, const char* key) {
    Style prefix = colorStyle(0xFFFF55);
    prefix.bold = 1;
    Text t(s.res.translate("debug.prefix"), prefix);
    t.append(" " + s.res.translate(key));
    s.addChat(t);
}

namespace {

bool worldKey(UiState& s, const InputEvent& e) {
    if (e.type != InputEvent::Key || e.repeat) return false;
    const int overlay = s.settings.key(Key::DebugOverlay), modifier = s.settings.key(Key::DebugModifier);
    if (overlay && e.code == overlay && overlay != modifier) {  // its own key: toggles on the press
        if (e.down) s.debug = !s.debug;
        return true;
    }
    if (modifier && e.code == modifier) {
        if (e.down) {
            sw.f3Down = true;
        } else {
            if (!sw.usedAsModifier && overlay == modifier) s.debug = !s.debug;  // F3 released alone
            sw.usedAsModifier = false;
            sw.f3Down = false;
        }
        return true;
    }
    if (!e.down || !modifier || !s.window->keyDown(modifier)) return false;
    if (s.isKey(Key::DebugSwitchGameMode, e)) {  // GameModeSwitcherScreen
        sw.usedAsModifier = true;
        if (s.online()) s.openScreen(Screen::GameModeSwitcher);
        return true;
    }
    if (s.isKey(Key::DebugSpectate, e)) {  // spectator <-> the previous game mode
        sw.usedAsModifier = true;
        if (s.gameMode() == 3) sendGameMode(s, sw.previousMode >= 0 && sw.previousMode != 3 ? sw.previousMode : 1);
        else sendGameMode(s, 3);
        return true;
    }
    if (s.isKey(Key::DebugShowAdvancedTooltips, e)) {
        sw.usedAsModifier = true;
        s.settings.advancedItemTooltips = !s.settings.advancedItemTooltips;
        debugFeedback(s, s.settings.advancedItemTooltips ? "debug.advanced_tooltips.on" : "debug.advanced_tooltips.off");
        s.settings.save();
        return true;
    }
    if (s.isKey(Key::DebugFocusPause, e)) {
        sw.usedAsModifier = true;
        s.settings.pauseOnLostFocus = !s.settings.pauseOnLostFocus;
        debugFeedback(s, s.settings.pauseOnLostFocus ? "debug.pause_focus.on" : "debug.pause_focus.off");
        s.settings.save();
        return true;
    }
    if (s.isKey(Key::DebugCopyRecreateCommand, e)) {  // F3 + I (debug_packets.cpp); with Shift: the client's data
        sw.usedAsModifier = true;
        copyRecreateCommand(s, !s.window->keyDown(VK_SHIFT));
        return true;
    }
    if (s.isKey(Key::DebugClearChat, e)) {  // ChatComponent.clearMessages(false): the sent history stays
        sw.usedAsModifier = true;
        s.chatLines.clear();
        s.chatMessages.clear();
        return true;
    }
    if (debugScreenKey(s, e)) {  // debug_screen.cpp: the other combinations
        sw.usedAsModifier = true;
        return true;
    }
    return false;
}

const UiModuleRegistrar kModule({"bed_gamemode", handlers, attached, tick, nullptr, nullptr, worldKey});

// --- InBedChatScreen ----------------------------------------------------------------------------------------------

void sendWakeUp(UiState& s) {  // ServerboundPlayerCommandPacket STOP_SLEEPING
    if (!s.online() || !s.session) return;
    net::Writer w;
    w.varInt(s.session->entityId());
    w.varInt(0);  // Action.STOP_SLEEPING
    w.varInt(0);
    s.client->sendPlay("player_command", w);
}

void bedBuild(UiState& s) {
    s.buttons.clear();
    Button b;
    b.x = float(s.guiW() / 2 - 100), b.y = float(s.guiH() - 40), b.w = 200;
    b.label = Text(s.res.translate("multiplayer.stopSleeping"));
    UiState* sp = &s;
    b.onPress = [sp] { sendWakeUp(*sp); };
    s.buttons.push_back(std::move(b));
}

void bedDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    s.drawChat(c, true);
    c.fill(2, float(H - 14), float(W - 2), float(H - 2), withAlpha(0, 0.5f));
    s.drawTextField(c, s.chatInput, 4, float(H - 12 + 2), float(W - 8), 0xFFE0E0E0, true, true);
    s.drawButtons(c);
}

void bedInput(UiState& s, const InputEvent& e) {
    if (s.buttonsInput(e)) return;
    if (e.type == InputEvent::Key && e.down) {
        if (e.code == VK_ESCAPE) return sendWakeUp(s);  // onClose: the screen closes when the server wakes us
        if (e.code == VK_RETURN) {
            const std::string text = s.chatInput.value;
            s.chatInput = UiState::TextField{};
            s.sendChat(text);
            return;
        }
    }
    s.textFieldKey(s.chatInput, e);
}

const ScreenRegistrar kBedScreen({Screen::InBed, bedBuild, bedDraw, bedInput, nullptr});

// --- GameModeSwitcherScreen -----------------------------------------------------------------------------------------

void switcherBuild(UiState& s) {
    s.buttons.clear();
    const int mode = s.gameMode();
    const int def = sw.previousMode >= 0 ? sw.previousMode : mode == 1 ? 0 : 1;  // getDefaultSelected
    sw.hovered = iconOf(def);
    sw.firstMouseSet = false;
}

void switcherDraw(UiState& s, Canvas& c) {
    const int W = c.guiWidth(), H = c.guiHeight();
    const float mx = s.mouseX, my = s.mouseY;
    blitTexture(c, s.sprites.texture("gui/container/gamemode_switcher"), float(W / 2 - 62), float(H / 2 - 31 - 27), 0, 0, 125,
                75, 128, 128);
    s.drawCentered(c, Text(s.res.translate(kIconName[sw.hovered])), float(W / 2), float(H / 2 - 31 - 20), 0xFFFFFFFF);
    const Text key = translatable("debug.gamemodes.select_next", {Text("F4", colorStyle(0x55FFFF))}, s.lang());
    s.drawCentered(c, key, float(W / 2), float(H / 2 + 5), 0xFFFFFFFF);
    if (!sw.firstMouseSet) sw.firstMouse[0] = mx, sw.firstMouse[1] = my, sw.firstMouseSet = true;
    const bool sameAsFirst = sw.firstMouse[0] == mx && sw.firstMouse[1] == my;
    const int allW = 4 * 31 - 5;
    for (int i = 0; i < 4; ++i) {
        const float x = float(W / 2 - allW / 2 + i * 31), y = float(H / 2 - 31);
        blitSprite(c, s.sprites.sprite("gamemode_switcher/slot"), x, y, 26, 26);
        if (sw.hovered == i) blitSprite(c, s.sprites.sprite("gamemode_switcher/selection"), x, y, 26, 26);
        ItemStack icon;
        icon.item = s.items.id(kIconItem[i]), icon.count = 1;
        s.drawItem(c, icon, x + 5, y + 5, false);
        if (!sameAsFirst && mx >= x && my >= y && mx < x + 26 && my < y + 26) sw.hovered = i;
    }
}

void switcherApply(UiState& s) {
    sendGameMode(s, kIconMode[sw.hovered]);
    s.closeScreen(false);
    sw.usedAsModifier = false;
    sw.f3Down = false;
}

void switcherInput(UiState& s, const InputEvent& e) {
    if (s.isKey(Key::DebugSwitchGameMode, e)) {
        sw.firstMouseSet = false;
        sw.hovered = (sw.hovered + 1) % 4;
        return;
    }
    if (e.type == InputEvent::Key && !e.down && e.code == s.settings.key(Key::DebugModifier)) return switcherApply(s);
    if (e.type == InputEvent::Key && e.down && e.code == VK_ESCAPE) {
        s.closeScreen(false);
        sw.usedAsModifier = false;
    }
}

const ScreenRegistrar kSwitcherScreen({Screen::GameModeSwitcher, switcherBuild, switcherDraw, switcherInput, nullptr});
}  // namespace

}  // namespace ui
