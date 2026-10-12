#pragma once
// The narrator (GameNarrator over the system's text-to-speech): Windows SAPI 5 voices on a worker thread. Chat,
// system messages and the widget under the mouse are spoken by the Narrator option (Narrates All / Chat / System),
// Ctrl+B cycles it (Narrator Hotkey), with the game's toast.
#include "ui/ui_state.h"

namespace ui::narrator {

bool available();  // a voice could be created (else the option shows "Not Available")
void say(const std::string& text, bool interrupt);
void statusChanged(UiState& s);                         // GameNarrator.updateNarratorStatus: speech + toast
void chat(UiState& s, const Text& message, int source);  // a chat line (GuiMessageSource 0 / 1 system, 2 player)
void system(UiState& s, const Text& message);            // saySystemQueued (action bar, titles)
void frame(UiState& s);                                  // the hovered widget of the open screen (Screen narration)
bool hotkey(UiState& s, const Window::InputEvent& e);    // Ctrl+B (KeyboardHandler); true: consumed

}  // namespace ui::narrator
