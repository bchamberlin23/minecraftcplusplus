#pragma once
// Server dialogs (dialogs.cpp): show_dialog / clear_dialog in the configuration and play phases, the dialog screens
// (notice, confirmation, multi_action, dialog_list, server_links) with their inputs and actions, server links, the
// waiting-for-response and warning screens, and the game's click event handling shared with the chat.
#include "ui/modules.h"
#include "ui/text.h"

#include <functional>
#include <string>

namespace ui {

// A ClickEvent: action ("open_url", "run_command", "suggest_command", "copy_to_clipboard", "change_page",
// "show_dialog", "custom"), its value (url / command / text / page / custom id) and NBT (show_dialog's dialog,
// custom's payload).
struct ClickAction {
    std::string action, value, nbt;
};
inline ClickAction clickOf(const TextEvents& ev) { return {ev.click, ev.clickValue, ev.clickNbt}; }

// Screen.defaultHandleGameClickEvent from a screen that is not a dialog (the chat): `back` is that screen.
void handleGameClick(UiState& s, const ClickAction& click, Screen back);
// DialogScreen.isPauseScreen while Screen::Dialog is open.
bool dialogPausesGame();
// PauseScreen.getCustomAdditions: the pause menu's dialog button (custom options or the server links); false: none.
bool pauseMenuDialogButton(UiState& s, Text& label, std::function<void()>& open);

}  // namespace ui
