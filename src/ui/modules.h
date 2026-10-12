#pragma once
// Self-registering UI parts. A .cpp file adds itself with a static registrar, so features live in their own files
// and the core (ui.cpp, hud.cpp, screens.cpp) only calls the registries:
//  - UiModule: play-packet handlers, a 20 Hz tick, HUD drawing, keys pressed in the world (no screen open);
//  - ScreenDef: a full screen (Screen enum value) with its own buttons, drawing and input;
//  - StationDef: a server menu type ("anvil", "merchant"...) drawn by the generic container screen with its own
//    layout, backgrounds, widgets, input and shift-click rule.
// Per-feature state lives in the feature's file (only the main thread touches it; packet handlers post to it).
#include "platform/window.h"
#include "ui/ui.h"

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace ui {

class Canvas;
struct Menu;
struct Layout;
struct ItemStack;
enum class Screen;
using UiState = Ui::State;
using InputEvent = Window::InputEvent;

struct UiModule {
    const char* name = "";
    void (*registerHandlers)(UiState& s, net::Client& client) = nullptr;  // before Client::start (network thread)
    void (*attached)(UiState& s) = nullptr;   // a new session (or none): forget per-connection state
    void (*tick)(UiState& s) = nullptr;       // 20 Hz with the HUD
    void (*drawHud)(UiState& s, Canvas& c) = nullptr;  // HUD layer, before the chat (not while F1 hides the HUD)
    void (*drawHudTop)(UiState& s, Canvas& c) = nullptr;  // over the chat (player list)
    bool (*worldKey)(UiState& s, const InputEvent& e) = nullptr;  // input while no screen is open; true: consumed
    void (*drawTop)(UiState& s, Canvas& c) = nullptr;  // over everything, screens and the title screen too (toasts)
    void (*frame)(UiState& s) = nullptr;  // every frame before the input, in every instance (title screen, world)
};
struct UiModuleRegistrar {
    explicit UiModuleRegistrar(const UiModule& m);
};
const std::vector<UiModule>& uiModules();

struct ScreenDef {
    Screen screen;
    void (*build)(UiState& s) = nullptr;                       // (re)build its buttons (opened, resized)
    void (*draw)(UiState& s, Canvas& c) = nullptr;            // everything (background included)
    void (*input)(UiState& s, const InputEvent& e) = nullptr;  // all input while open
    void (*closed)(UiState& s) = nullptr;                     // left (closeScreen or another screen)
};
struct ScreenRegistrar {
    explicit ScreenRegistrar(const ScreenDef& d);
};
const ScreenDef* screenDef(Screen s);

struct StationDef {
    // Slot positions, background texture (+ size), label positions. Layout::playerStart < 0: no player inventory.
    void (*layout)(const Menu& m, Layout& l) = nullptr;
    void (*opened)(UiState& s, Menu& m) = nullptr;  // the screen opened: reset this screen's state
    // Drawn after the background (before slots), after the slots and labels (before the carried item), and last.
    void (*drawBg)(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) = nullptr;
    void (*drawFg)(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) = nullptr;
    void (*drawTop)(UiState& s, Canvas& c, Menu& m, const Layout& l, float left, float top) = nullptr;
    // Before the generic container input (slots, Esc / E closes); true: consumed.
    bool (*input)(UiState& s, const InputEvent& e, Menu& m, const Layout& l, float left, float top) = nullptr;
    int resultSlot = -1;  // an output slot: never placed into, taken whole
    // quickMoveStack: moveTo(start, end, backwards) like moveItemStackTo. Return 1 moved, 0 nothing, -1 generic rule,
    // 3 moved but the shift-click ends (the game returns EMPTY after moving).
    int (*quickMove)(UiState& s, const Menu& m, int index, const ItemStack& st,
                     const std::function<bool(int start, int end, bool backwards)>& moveTo) = nullptr;
    bool (*mayPlace)(UiState& s, const Menu& m, int slot, const ItemStack& st) = nullptr;  // Slot.mayPlace
    // Slot.getMaxStackSize of the station's own slots (enchanting item 1, beacon payment 1, ...); 0: the default.
    int (*slotMax)(UiState& s, const Menu& m, int slot, const ItemStack& st) = nullptr;
    bool (*mayPickup)(UiState& s, const Menu& m, int slot) = nullptr;  // Slot.mayPickup (anvil result: the level cost)
    // The result slot's onTake: what taking the result uses up (stonecutter input, loom banner and dye, ...).
    void (*onTake)(UiState& s, Menu& m) = nullptr;
    bool partialResult = false;  // the result slot's container gives part of its stack (furnace, loom), not all of it
    int pickAll = 1;  // canTakeItemForPickAll: 1 every slot, 0 all but the result slot, -1 none (merchant)
};
struct StationRegistrar {
    StationRegistrar(std::initializer_list<const char*> menuTypes, const StationDef& def);
};
const StationDef* stationFor(const std::string& menuType);  // "anvil" (minecraft:menu key without namespace)

// WinScreen: the End poem (and credits) after leaving the End, or the credits alone (credits.cpp).
void openWinScreen(UiState& s, bool poem);

// Sends ServerboundContainerButtonClickPacket for the open container.
void sendContainerButton(UiState& s, int buttonId);
// F3 + I: the recreate command of the block / entity under the crosshair to the clipboard (debug_packets.cpp).
void copyRecreateCommand(UiState& s, bool pullFromServer);

}  // namespace ui
